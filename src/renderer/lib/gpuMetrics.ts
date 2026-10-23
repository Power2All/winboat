const fs: typeof import("fs") = require("node:fs");
const path: typeof import("path") = require("node:path");
const childProcess: typeof import("child_process") = require("node:child_process");
const { promisify }: typeof import("util") = require("node:util");
import { detectHostGpu } from "./virtualGpu";

const execFileAsync = promisify(childProcess.execFile);

/**
 * The VM's use of the host GPU. Windows can't tell (the virtual GPU driver doesn't report its engines' busy time, its
 * counters stay at 0), and the work is done on the host anyway: virglrenderer runs it in QEMU's process. NVIDIA's
 * driver reports per process (nvidia-smi pmon); other drivers report per DRM client in /proc/<pid>/fdinfo, which only
 * root can read for QEMU's process: it's read inside the container.
 */
export type GpuMetrics = {
    state: "off" | "measuring" | "ok" | "unavailable";
    usage: number; // % of the GPU's time the VM uses
    name: string; // the host GPU
    memoryUsed: number | null; // MB of video memory the VM uses
    hostUsage: number | null; // % of the time the GPU is busy, the host's own apps included
    commandsPerSecond: number | null; // commands Windows' driver puts on the virtual GPU's queue (QEMU counts them)
};

// Seconds between samples, and samples per helper process: they end by themselves (WinBoat may quit without ending
// them) and are started again while somebody watches
const SAMPLE_S = 2;
const SAMPLES_PER_RUN = 30;

/**
 * Prints QEMU's count of the virtio GPU's control queue ("used <n>": its used index, 16 bits) every SAMPLE_S seconds,
 * through dockur's monitor socket in the container. It connects for each sample only: dockur uses the socket too
 * (to shut Windows down). Not with the GPU in a process of its own (vhost-user-vga): QEMU reads that queue's state
 * from the GPU process with GET_VRING_BASE, which stops the queue (Windows' GPU stopped every second), and its
 * commands don't go through QEMU anyway.
 */
const QUEUE_SCRIPT = `
import re, socket, time
def hmp(command):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(2)
    s.connect("/run/shm/monitor.sock")
    def read():
        out = b""
        while not out.endswith(b"(qemu) "):
            data = s.recv(65536)
            if not data:
                break
            out += data
        return out.decode(errors="replace")
    try:
        read()
        s.sendall(command.encode() + b"\\n")
        return read()
    finally:
        s.close()
try:
    with open("/proc/%s/cmdline" % open("/run/shm/qemu.pid").read().strip(), "rb") as f:
        if b"vhost-user" in f.read():
            # Nothing to count: a run's worth of time without samples (exiting at once restarts it at once)
            time.sleep(${SAMPLES_PER_RUN * SAMPLE_S})
            raise SystemExit
except OSError:
    pass
path = None
for i in range(${SAMPLES_PER_RUN}):
    try:
        if path is None:
            m = re.search(r"(/machine/\\S+)\\s+\\[virtio-gpu", hmp("info virtio"))
            path = m and m.group(1)
        if path:
            m = re.search(r"used_idx:\\s+(\\d+)", hmp("info virtio-queue-status %s 0" % path))
            if m:
                print("used", m.group(1), flush=True)
    except Exception:
        pass
    time.sleep(${SAMPLE_S})
`;

/**
 * QEMU's process of the container (on the host), found by the container's id in its cgroup. Its command line tells
 * it's QEMU: its name is the VM's (dockur: -name windows,process=windows).
 */
async function qemuPid(executable: string, containerName: string): Promise<number | null> {
    const { stdout } = await execFileAsync(executable, ["inspect", "-f", "{{.Id}}", containerName]);
    const id = stdout.trim();
    if (!id) return null;
    for (const name of fs.readdirSync("/proc")) {
        if (!/^\d+$/.test(name)) continue;
        try {
            const program = fs.readFileSync(`/proc/${name}/cmdline`, "utf-8").split("\0")[0];
            if (!path.basename(program).startsWith("qemu-system")) continue;
            if (fs.readFileSync(`/proc/${name}/cgroup`, "utf-8").includes(id)) return parseInt(name);
        } catch {}
    }
    return null;
}

/** Calls onLine with every line a helper prints until it ends, then starts it again (until stopped) */
function keepRunning(
    command: string,
    args: string[],
    onLine: (line: string) => void,
    onFailed: () => void,
): { stop: () => void } {
    let stopped = false;
    let child: import("child_process").ChildProcess | null = null;
    const start = () => {
        if (stopped) return;
        const startedAt = Date.now();
        child = childProcess.spawn(command, args, { stdio: ["ignore", "pipe", "ignore"] });
        let pending = "";
        child.stdout!.on("data", (data: Buffer) => {
            const lines = (pending + data.toString()).split("\n");
            pending = lines.pop()!;
            lines.forEach(onLine);
        });
        child.once("error", () => onFailed());
        child.once("exit", code => {
            if (stopped) return;
            // One that fails right away (no such program, the container stopped) is tried again less often
            if (code !== 0 && Date.now() - startedAt < 5000) {
                onFailed();
                setTimeout(start, 10_000);
            } else {
                start();
            }
        });
    };
    start();
    return {
        stop: () => {
            stopped = true;
            child?.kill();
        },
    };
}

/** Columns of a "# gpu pid type sm ..." header line */
function columns(header: string): string[] {
    return header.replace(/^#/, "").trim().split(/\s+/);
}

/** Measures the VM's GPU use while it runs (until stop), reporting each sample to onUpdate */
export class GpuMonitor {
    #metrics: GpuMetrics = {
        state: "measuring",
        usage: 0,
        name: "",
        memoryUsed: null,
        hostUsage: null,
        commandsPerSecond: null,
    };
    #helpers: { stop: () => void }[] = [];
    #pid: number | null = null;
    #pidCheck: NodeJS.Timeout | null = null;
    #stopped = false;

    constructor(
        private executable: string,
        private containerName: string,
        private renderNode: string | undefined,
        private onUpdate: (metrics: GpuMetrics) => void,
    ) {
        const host = detectHostGpu();
        const node = host.renderNodes.find(n => n.node === renderNode) ?? host.renderNodes[0];
        this.#metrics.name = node ? `${node.vendor} GPU` : "GPU";
        void this.#findPid();
        // QEMU restarts with the container
        this.#pidCheck = setInterval(() => {
            if (!this.#pid || !fs.existsSync(`/proc/${this.#pid}`)) void this.#findPid();
        }, 10_000);
        this.#watchQueue();
        if (node?.vendorId === "0x10de") this.#watchNvidia();
        else if (node) this.#watchFdinfo(node.node);
        else this.#update({ state: "unavailable" });
    }

    stop() {
        this.#stopped = true;
        this.#helpers.forEach(helper => helper.stop());
        if (this.#pidCheck) clearInterval(this.#pidCheck);
    }

    async #findPid() {
        try {
            this.#pid = await qemuPid(this.executable, this.containerName);
        } catch {
            this.#pid = null;
        }
    }

    #update(changes: Partial<GpuMetrics>) {
        if (this.#stopped) return;
        this.#metrics = { ...this.#metrics, ...changes };
        this.onUpdate(this.#metrics);
    }

    /** QEMU's count of the commands on the virtio GPU's control queue: commands per second */
    #watchQueue() {
        let previous: { used: number; at: number } | null = null;
        this.#helpers.push(
            keepRunning(
                this.executable,
                ["exec", this.containerName, "python3", "-c", QUEUE_SCRIPT],
                line => {
                    const match = line.match(/^used (\d+)$/);
                    if (!match) return;
                    const sample = { used: parseInt(match[1]), at: Date.now() };
                    if (previous && sample.at > previous.at) {
                        // The index counts in 16 bits
                        const commands = (sample.used - previous.used + 65536) % 65536;
                        this.#update({ commandsPerSecond: Math.round((commands * 1000) / (sample.at - previous.at)) });
                    }
                    previous = sample;
                },
                () => this.#update({ commandsPerSecond: null }),
            ),
        );
    }

    /** nvidia-smi pmon: every process's share of the GPU (sm %) and video memory (fb MB) */
    #watchNvidia() {
        let header: string[] = [];
        let dmonHeader: string[] = [];
        let gpuIndex = "0";
        const failed = () => this.#update({ state: "unavailable" });
        this.#helpers.push(
            keepRunning(
                "nvidia-smi",
                ["pmon", "-s", "um", "-d", `${SAMPLE_S}`, "-c", `${SAMPLES_PER_RUN}`],
                line => {
                    // The header comes once; every sample lists every process using the GPU ("-": idle)
                    if (line.startsWith("# gpu")) {
                        header = columns(line);
                        return;
                    }
                    if (line.startsWith("#") || !header.length) return;
                    const fields = line.trim().split(/\s+/);
                    const value = (column: string) => fields[header.indexOf(column)];
                    if (!this.#pid || value("pid") !== `${this.#pid}`) return;
                    gpuIndex = value("gpu") ?? "0";
                    const number = (text: string | undefined) => (text && /^\d+$/.test(text) ? parseInt(text) : 0);
                    this.#update({ state: "ok", usage: number(value("sm")), memoryUsed: number(value("fb")) });
                },
                failed,
            ),
            // Every GPU's own load (sm %), the host's apps included
            keepRunning(
                "nvidia-smi",
                ["dmon", "-s", "u", "-d", `${SAMPLE_S}`, "-c", `${SAMPLES_PER_RUN}`],
                line => {
                    if (line.startsWith("# gpu")) {
                        dmonHeader = columns(line);
                        return;
                    }
                    if (line.startsWith("#") || !dmonHeader.length) return;
                    const fields = line.trim().split(/\s+/);
                    if (fields[dmonHeader.indexOf("gpu")] !== gpuIndex) return;
                    const usage = fields[dmonHeader.indexOf("sm")];
                    this.#update({ hostUsage: /^\d+$/.test(usage) ? parseInt(usage) : null });
                },
                failed,
            ),
        );
        execFileAsync("nvidia-smi", ["--query-gpu=index,name", "--format=csv,noheader"])
            .then(({ stdout }) => {
                for (const line of stdout.split("\n")) {
                    const [index, name] = line.split(",").map(field => field.trim());
                    if (index === gpuIndex && name) this.#update({ name });
                }
            })
            .catch(() => {});
    }

    /**
     * /proc/<QEMU>/fdinfo/<fd> of its DRM clients (amdgpu, i915, xe, ...): "drm-engine-<engine>: <ns> ns" busy time
     * (xe: "drm-cycles-<engine>" of "drm-total-cycles-<engine>") and "drm-memory-vram" / "drm-resident-vram*". Read
     * inside the container (it runs as root there), every SAMPLE_S seconds, "@@" after each sample.
     */
    #watchFdinfo(renderNode: string) {
        const script =
            `i=0; while [ $i -lt ${SAMPLES_PER_RUN} ]; do ` +
            `for p in /proc/[0-9]*; do case "$(head -c 64 $p/cmdline 2>/dev/null | tr '\\0' ' ')" in ` +
            `qemu-system*|*/qemu-system*) grep -h -e '^drm-' -e '^pos:' $p/fdinfo/* 2>/dev/null;; esac; done; ` +
            `echo @@; i=$((i+1)); sleep ${SAMPLE_S}; done`;
        type Sample = { at: number; engines: Map<string, number>; cycles: Map<string, [number, number]> };
        let previous: Sample | null = null;
        let clients = new Map<string, Map<string, string>>(); // drm-client-id -> its drm- lines
        let fd = new Map<string, string>();
        const busyFile = path.join("/sys/class/drm", path.basename(renderNode), "device/gpu_busy_percent");

        const finishFd = () => {
            const id = fd.get("drm-client-id");
            if (id) clients.set(id, fd); // file descriptors of one client report it alike
            fd = new Map();
        };
        this.#helpers.push(
            keepRunning(
                this.executable,
                ["exec", this.containerName, "sh", "-c", script],
                line => {
                    if (line.startsWith("pos:")) {
                        finishFd();
                    } else if (line.startsWith("drm-")) {
                        const [key, ...rest] = line.split(":");
                        fd.set(key.trim(), rest.join(":").trim());
                    } else if (line.trim() === "@@") {
                        finishFd();
                        const sample: Sample = { at: Date.now(), engines: new Map(), cycles: new Map() };
                        let memoryKiB: number | null = null;
                        for (const lines of clients.values()) {
                            for (const [key, text] of lines) {
                                const value = parseInt(text);
                                if (isNaN(value)) continue;
                                let match: RegExpMatchArray | null;
                                if ((match = key.match(/^drm-engine-(.+)$/))) {
                                    sample.engines.set(match[1], (sample.engines.get(match[1]) ?? 0) + value);
                                } else if ((match = key.match(/^drm-(total-)?cycles-(.+)$/))) {
                                    const pair = sample.cycles.get(match[2]) ?? [0, 0];
                                    pair[match[1] ? 1 : 0] += value;
                                    sample.cycles.set(match[2], pair);
                                } else if (/^drm-(memory|resident)-(vram|local)/.test(key)) {
                                    const kib = text.endsWith("MiB") ? value * 1024 : text.endsWith("GiB")
                                        ? value * 1024 * 1024 : text.endsWith("KiB") ? value : value / 1024;
                                    memoryKiB = (memoryKiB ?? 0) + kib;
                                }
                            }
                        }
                        clients = new Map();
                        if (previous) {
                            let usage = 0;
                            const elapsedNs = (sample.at - previous.at) * 1e6;
                            for (const [engine, busy] of sample.engines) {
                                const before = previous.engines.get(engine);
                                if (before !== undefined && elapsedNs > 0) {
                                    usage = Math.max(usage, ((busy - before) / elapsedNs) * 100);
                                }
                            }
                            for (const [engine, [cycles, total]] of sample.cycles) {
                                const before = previous.cycles.get(engine);
                                if (before && total > before[1]) {
                                    usage = Math.max(usage, ((cycles - before[0]) / (total - before[1])) * 100);
                                }
                            }
                            const hostBusy = parseInt(fs.existsSync(busyFile) ? fs.readFileSync(busyFile, "utf-8") : "");
                            this.#update({
                                state: "ok",
                                usage: Math.round(Math.min(100, Math.max(0, usage))),
                                memoryUsed: memoryKiB === null ? null : Math.round(memoryKiB / 1024),
                                hostUsage: isNaN(hostBusy) ? null : hostBusy,
                            });
                        }
                        previous = sample;
                    }
                },
                () => this.#update({ state: "unavailable" }),
            ),
        );
    }
}
