const fs: typeof import("fs") = require("node:fs");
const path: typeof import("path") = require("node:path");
const childProcess: typeof import("child_process") = require("node:child_process");
const { promisify }: typeof import("util") = require("node:util");
const remote: typeof import("@electron/remote") = require("@electron/remote");
import type { ComposeConfig } from "../../types";
import { ContainerRuntimes } from "./containers/common";
import { WINBOAT_DIR } from "./constants";
import type { SeamlessBackground } from "./config";

const execFileAsync = promisify(childProcess.execFile);

/**
 * Virtual GPU (experimental): Windows renders through a virtio-gpu device on the host's GPU (any vendor), using
 * QEMU's virglrenderer on the host and the viogpu3d driver bundled with the guest server in Windows. OpenGL,
 * Direct3D 9 and Direct3D 10/11 (feature level 10_0); no Vulkan or Direct3D 12 yet. See docs/hardware-acceleration.md.
 */

// The first dockur version that turns GPU=Y on for Windows guests (and on AMD CPUs) and picks the render node
export const VIRTUAL_GPU_IMAGE = "ghcr.io/dockur/windows:6.05";
const VIRTUAL_GPU_MIN_VERSION = [6, 5];

// The toolkit mounts the NVIDIA libraries at the host's paths (/usr/lib on Arch, /usr/lib64 on Fedora), but the
// container's Debian Mesa only looks for GBM backends in its multiarch directory
const NVIDIA_GBM_BACKENDS_PATH = "/usr/lib/x86_64-linux-gnu/gbm:/usr/lib/gbm:/usr/lib64/gbm";
const NVIDIA_CDI_DEVICE = "nvidia.com/gpu=all";
const DRI_DEVICE = "/dev/dri";

const GPU_VENDORS: Record<string, string> = {
    "0x10de": "NVIDIA",
    "0x1002": "AMD",
    "0x8086": "Intel",
};

/**
 * How Windows' screen reaches Linux. RDP: each app in its own window (RemoteApp), about 30 fps. SPICE: QEMU hands the
 * whole desktop to a local viewer as a GPU buffer (zero-copy, 60 fps, lower latency), no separate app windows.
 *
 * SPICE needs spice-server 0.16 (its GL_SCANOUT2 carries the buffer's tiling, which NVIDIA needs; dockur 6.05 has
 * 0.15), so the VM runs an image built locally from spice/image/Dockerfile, and a viewer with spice-gtk 0.43, which
 * WinBoat bundles (spice/viewer). dockur's GPU=Y always adds egl-headless, which can't share the screen with SPICE GL,
 * so the GPU device and SPICE go in ARGUMENTS with dockur's display off. Windows allows one session per user: an RDP
 * connection would take over the desktop SPICE shows, so in SPICE mode apps open the desktop instead.
 */
export enum DisplayRoute {
    RDP = "rdp",
    SPICE = "spice",
}

export const SPICE_IMAGE = "winboat-windows-spice:6.05-29";
// Every version of it (older WinBoats built older ones)
const SPICE_IMAGE_NAME = SPICE_IMAGE.slice(0, SPICE_IMAGE.indexOf(":") + 1);
const SPICE_HOST_DIR = path.join(WINBOAT_DIR, "spice");
const SPICE_SOCKET = path.join(SPICE_HOST_DIR, "spice.sock");
const SPICE_VOLUME = `${SPICE_HOST_DIR}:/spice`;
/**
 * The GPU runs in a process of its own (QEMU's vhost-user-gpu, which the image starts for this device): with QEMU's
 * own virtio-vga-gl, the GPU's work ran on QEMU's main loop and held up the sound card and the vCPUs while Windows drew
 * a lot (crackling sound). hostmem is the device's host-visible memory window, where Vulkan (Venus) maps its memory
 * (only address space: the host maps a buffer there when Windows asks for one)
 */
const SPICE_GPU_DEVICE =
    "-device vhost-user-vga,chardev=winboat-gpu,hostmem=4G -chardev socket,id=winboat-gpu,path=/run/shm/winboat-gpu.sock";
const SPICE_ARGUMENTS_START = `${SPICE_GPU_DEVICE} -spice unix=on,addr=/spice/spice.sock`;
// What older WinBoats set up: 0.9.2-gpu.28 to gpu.37 (no Vulkan), and gpu.27 and older (the GPU in QEMU itself)
const OLD_SPICE_ARGUMENTS_STARTS = [
    "-device vhost-user-vga,chardev=winboat-gpu -chardev socket,id=winboat-gpu,path=/run/shm/winboat-gpu.sock " +
        "-spice unix=on,addr=/spice/spice.sock",
    "-device virtio-vga-gl -spice unix=on,addr=/spice/spice.sock",
];

function spiceArgumentsStart(args: string): number {
    for (const start of [SPICE_ARGUMENTS_START, ...OLD_SPICE_ARGUMENTS_STARTS]) {
        const index = args.indexOf(start);
        if (index >= 0) return index;
    }
    return -1;
}
const SPICE_DEVICE_ARGUMENTS = [
    // The SPICE agent's channel (resizing, clipboard; the agent comes with the Windows driver)
    "-device virtio-serial-pci -chardev spicevmc,id=vdagent,name=vdagent",
    "-device virtserialport,chardev=vdagent,name=com.redhat.spice.0",
    // Sound over SPICE (RDP brings its own); Windows has the HD Audio driver
    "-audiodev spice,id=spiceaudio -device ich9-intel-hda -device hda-duplex,audiodev=spiceaudio",
    // Seamless app windows: the Guest Server's window tracker reports an app's windows to the viewer
    "-chardev spiceport,id=winboatwin,name=org.winboat.windows",
    "-device virtserialport,chardev=winboatwin,name=org.winboat.windows",
].join(" ");

function spiceArguments(renderNode: string): string {
    return `${SPICE_ARGUMENTS_START},disable-ticketing=on,gl=on,rendernode=${renderNode} ${SPICE_DEVICE_ARGUMENTS}`;
}

// WinBoat appends its SPICE options last, so they run to the end of ARGUMENTS (whichever version added them)
function withoutSpiceArguments(args: string): string {
    const start = spiceArgumentsStart(args);
    return start < 0 ? args : args.slice(0, start).trim();
}

/**
 * The SPICE route was set up by an older WinBoat (without sound, or on an older image, for example): applying it
 * again updates it
 */
export function spiceRouteOutdated(compose: ComposeConfig): boolean {
    const service = compose.services.windows;
    const args = service.environment.ARGUMENTS ?? "";
    return (
        spiceArgumentsStart(args) >= 0 &&
        (!args.includes(SPICE_ARGUMENTS_START) ||
            !args.endsWith(SPICE_DEVICE_ARGUMENTS) ||
            service.image !== SPICE_IMAGE)
    );
}

export function displayRoute(compose: ComposeConfig): DisplayRoute {
    return spiceArgumentsStart(compose.services.windows.environment.ARGUMENTS ?? "") >= 0
        ? DisplayRoute.SPICE
        : DisplayRoute.RDP;
}

/** Where WinBoat's SPICE files are: spice/image (the VM image's Dockerfile) and spice/viewer (the bundled viewer) */
function spiceResources(): string {
    return remote.app.isPackaged
        ? path.join(process.resourcesPath, "spice")
        : path.join(remote.app.getAppPath(), "..", "..", "spice");
}

export function spiceViewerBundled(): boolean {
    return fs.existsSync(path.join(spiceResources(), "viewer", "bin", "winboat-spice-viewer"));
}

export async function spiceImageExists(executable: string): Promise<boolean> {
    try {
        await execFileAsync(executable, ["image", "inspect", SPICE_IMAGE]);
        return true;
    } catch {
        return false;
    }
}

/** Builds the SPICE VM image (a few minutes, once); onOutput gets the build's output lines */
export function buildSpiceImage(executable: string, onOutput: (line: string) => void): Promise<void> {
    const dir = path.join(spiceResources(), "image");
    const args = [
        "build",
        "-t",
        SPICE_IMAGE,
        "--build-arg",
        `BASE_IMAGE=${VIRTUAL_GPU_IMAGE}`,
        "-f",
        path.join(dir, "Dockerfile"),
        dir,
    ];
    return new Promise((resolve, reject) => {
        const build = childProcess.spawn(executable, args, { env: { ...process.env, DOCKER_BUILDKIT: "0" } });
        const lines: string[] = [];
        const collect = (data: Buffer) => {
            for (const line of data
                .toString()
                .split("\n")
                .map(l => l.trim())
                .filter(Boolean)) {
                lines.push(line);
                if (lines.length > 30) lines.shift();
                onOutput(line);
            }
        };
        build.stdout.on("data", collect);
        build.stderr.on("data", collect);
        build.on("error", reject);
        build.on("close", code =>
            code === 0 ? resolve() : reject(new Error(`Building the SPICE image failed:\n${lines.join("\n")}`)),
        );
    });
}

let spiceViewer: import("child_process").ChildProcess | null = null;
let spiceViewerTitle = "";

// The viewer showing an app's window, or quitting: what waitForAppWindow waits for
const viewerListeners = new Set<(shown: boolean) => void>();

function notifyViewerListeners(shown: boolean) {
    for (const listener of [...viewerListeners]) listener(shown);
}

// The frames per second the viewer reports ("fps <n>"), null when it quits
const fpsListeners = new Set<(fps: number | null) => void>();

/** Calls listener with the running viewer's frames per second (null: none runs); the returned function stops it */
export function onViewerFps(listener: (fps: number | null) => void): () => void {
    fpsListeners.add(listener);
    return () => fpsListeners.delete(listener);
}

function notifyFps(fps: number | null) {
    for (const listener of [...fpsListeners]) listener(fps);
}

/**
 * Waits for the seamless viewer to show an app's window (true), or to quit, or for timeoutMs (false); cancel stops
 * waiting. Called before the app starts.
 */
export function waitForAppWindow(timeoutMs: number): { shown: Promise<boolean>; cancel: () => void } {
    let cancel = () => {};
    const shown = new Promise<boolean>(resolve => {
        const done = (result: boolean) => {
            clearTimeout(timer);
            viewerListeners.delete(done);
            resolve(result);
        };
        const timer = setTimeout(() => done(false), timeoutMs);
        viewerListeners.add(done);
        cancel = () => done(false);
    });
    return { shown, cancel };
}

/**
 * DISPLAY and XAUTHORITY for XWayland. Electron drops both from its environment when it runs as a Wayland client,
 * so they're found again: the X socket, then the session's authorization file (KDE, GNOME, ~/.Xauthority), or else
 * another of the user's processes that has both.
 */
function xwaylandEnvironment(env: Record<string, string | undefined>): Record<string, string> {
    const result: Record<string, string> = {};
    if (!env.DISPLAY) {
        const socket = fs.existsSync("/tmp/.X11-unix")
            ? fs.readdirSync("/tmp/.X11-unix").find(name => /^X\d+$/.test(name))
            : undefined;
        if (socket) result.DISPLAY = `:${socket.slice(1)}`;
    }
    if (!env.XAUTHORITY) {
        const runtimeDir = env.XDG_RUNTIME_DIR || `/run/user/${process.getuid?.() ?? 1000}`;
        const candidates: string[] = [];
        try {
            for (const name of fs.readdirSync(runtimeDir)) {
                if (name.startsWith("xauth_") || name.startsWith(".mutter-Xwaylandauth.")) {
                    candidates.push(path.join(runtimeDir, name));
                }
            }
        } catch {}
        if (env.HOME) candidates.push(path.join(env.HOME, ".Xauthority"));
        const newest = candidates
            .filter(file => fs.existsSync(file))
            .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)[0];
        if (newest) {
            result.XAUTHORITY = newest;
        } else {
            for (const pid of fs.readdirSync("/proc").filter(name => /^\d+$/.test(name))) {
                try {
                    const vars = fs.readFileSync(`/proc/${pid}/environ`, "utf-8").split("\0");
                    const xauth = vars.find(v => v.startsWith("XAUTHORITY="));
                    if (xauth && vars.some(v => v.startsWith("DISPLAY="))) {
                        result.XAUTHORITY = xauth.slice("XAUTHORITY=".length);
                        break;
                    }
                } catch {}
            }
        }
    }
    return result;
}

// The seamless viewer's "title" (spiceViewerTitle): it shows the windows of apps, each in a window of its own
const SEAMLESS_VIEWER = "\0seamless";

/**
 * Starts the bundled SPICE viewer. One viewer at a time (a second client would take the connection over): another
 * one is replaced. Throws if it can't start.
 */
async function startSpiceViewer(
    executable: string,
    containerName: string,
    title: string,
    args: string[],
    onRefreshRate?: (rate: number) => void,
): Promise<import("child_process").ChildProcess> {
    if (spiceViewer && spiceViewer.exitCode === null) spiceViewer.kill();
    // QEMU runs as root in the container and creates the socket for root only
    await execFileAsync(executable, ["exec", containerName, "chmod", "666", "/spice/spice.sock"]);

    // The renderer's environment is Chromium's trimmed one (no XDG_RUNTIME_DIR, WAYLAND_DISPLAY, ...): the viewer
    // gets the main process's. Its sound goes to PipeWire or PulseAudio, which it finds in the session's runtime
    // directory (without it, it had no sound at all).
    // spice-gtk 0.43 draws black on NVIDIA under native Wayland, so the viewer uses X11 (XWayland). Its EGL too:
    // Chromium sets EGL_PLATFORM=wayland for itself, and NVIDIA's EGL then took the X11 display for a Wayland one
    // (the viewer crashed right away)
    const env: Record<string, string | undefined> = {
        ...remote.process.env,
        ...process.env,
        GDK_BACKEND: "x11",
        EGL_PLATFORM: "x11",
    };
    env.XDG_RUNTIME_DIR ||= `/run/user/${process.getuid?.() ?? 1000}`;
    env.DBUS_SESSION_BUS_ADDRESS ||= `unix:path=${env.XDG_RUNTIME_DIR}/bus`;
    delete env.LD_LIBRARY_PATH; // the AppImage's; the viewer finds its own libraries through its rpath
    Object.assign(env, xwaylandEnvironment(env));

    const logPath = path.join(WINBOAT_DIR, "spice-viewer.log");
    const log = fs.openSync(logPath, "a");
    fs.writeSync(
        log,
        `--- ${new Date().toISOString()} DISPLAY=${env.DISPLAY ?? ""} XAUTHORITY=${env.XAUTHORITY ?? ""} ` +
            `WAYLAND_DISPLAY=${env.WAYLAND_DISPLAY ?? ""}\n`,
    );
    const viewer = childProcess.spawn(
        path.join(spiceResources(), "viewer", "bin", "winboat-spice-viewer"),
        [...args, `spice+unix://${SPICE_SOCKET}`],
        { env, detached: true, stdio: ["pipe", "pipe", log] },
    );
    fs.closeSync(log);
    spiceViewer = viewer;
    spiceViewerTitle = title;
    viewer.unref();
    // Not a viewer this one replaced
    viewer.once("exit", () => {
        if (spiceViewer !== viewer) return;
        notifyViewerListeners(false);
        notifyFps(null);
    });
    // WinBoat may quit before the viewer: writes then fail, the viewer stays
    viewer.stdin!.on("error", () => {});
    // stdout: "refresh-rate <Hz>", "window-shown" (the whole desktop shows when it can't show windows) and
    // "fps <n>" lines, everything else goes to the log
    let pending = "";
    viewer.stdout!.on("data", (data: Buffer) => {
        const lines = (pending + data.toString()).split("\n");
        pending = lines.pop()!;
        for (const line of lines) {
            const rate = line.match(/^refresh-rate (\d+)$/);
            const fps = line.match(/^fps (\d+)$/);
            if (rate) onRefreshRate?.(parseInt(rate[1]));
            else if (fps) notifyFps(parseInt(fps[1]));
            else if (line === "window-shown") notifyViewerListeners(true);
            else if (line.trim()) fs.appendFileSync(logPath, line + "\n");
            if (line === "seamless-unavailable") notifyViewerListeners(true);
        }
    });

    // A viewer that can't start (no display, missing libraries) exits right away: report it
    const exit = await new Promise<string | null>(resolve => {
        const timer = setTimeout(() => resolve(null), 2000);
        viewer.once("error", error => (clearTimeout(timer), resolve(error.message)));
        viewer.once("exit", code => (clearTimeout(timer), resolve(`exit code ${code}`)));
    });
    if (exit !== null) {
        const tail = fs.readFileSync(logPath, "utf-8").trim().split("\n").slice(-4).join("\n");
        throw new Error(`The SPICE viewer didn't start (${exit}):\n${tail}`);
    }
    return viewer;
}

/**
 * Opens the bundled SPICE viewer on the VM's whole desktop in one window, titled after what it shows. A viewer that
 * shows it already stays.
 */
export async function openSpiceViewer(
    executable: string,
    containerName: string,
    title = "WinBoat",
    wmClass = "winboat-desktop",
    // The refresh rate (Hz) of the monitor the window is on, when it opens and whenever that changes
    onRefreshRate?: (rate: number) => void,
): Promise<void> {
    if (spiceViewer && spiceViewer.exitCode === null && spiceViewerTitle === title) return;
    await startSpiceViewer(executable, containerName, title, ["--title", title, "--class", wmClass], onRefreshRate);
}

/**
 * Shows the windows of an app started on the VM's desktop (its process id) as Linux windows of their own, like RDP's
 * RemoteApp: Windows' desktop gets the size of the primary monitor and the Guest Server's window tracker reports the
 * app's windows (and those of what it starts) to the viewer, which places a window where each of them is. The
 * viewer shows several apps, and quits when none runs anymore. Without the tracker (a VM set up by an older
 * WinBoat) it shows the whole desktop instead.
 */
export async function showSeamlessApp(
    executable: string,
    containerName: string,
    pid: number,
    background: SeamlessBackground,
    onRefreshRate?: (rate: number) => void,
): Promise<void> {
    // A running viewer keeps its background until it quits (it shows other apps' windows too)
    if (spiceViewer && spiceViewer.exitCode === null && spiceViewerTitle === SEAMLESS_VIEWER) {
        spiceViewer.stdin!.write(`track ${pid}\n`);
        return;
    }
    await startSpiceViewer(
        executable,
        containerName,
        SEAMLESS_VIEWER,
        ["--seamless", "--background", background, "--track", `${pid}`],
        onRefreshRate,
    );
}

export type RenderNode = {
    node: string; // /dev/dri/renderD128
    vendorId: string;
    vendor: string;
    driver: string; // kernel driver, e.g. nvidia, amdgpu, i915, xe
};

export type HostGpuInfo = {
    renderNodes: RenderNode[];
    nvidia: {
        present: boolean;
        // NVIDIA Container Toolkit: injects the driver's EGL/GBM libraries into the container
        containerToolkit: boolean;
        // podman: the toolkit's CDI specification (nvidia-ctk cdi generate)
        cdiSpec: boolean;
        // nvidia-drm.modeset=1: needed for the render node to work with GBM
        modeset: boolean;
    };
};

/** What the guest server's /gpu endpoint reports (guest_server/scripts/gpu-driver.ps1) */
export type GuestGpuStatus = {
    // fingerprint: the package's catalog hash (rebuilds keep the INF version)
    bundled: { present: boolean; version: string | null; fingerprint: string | null };
    installed: { present: boolean; version: string | null; package: string | null; fingerprint: string | null };
    device: { name: string; status: string; problem: number } | null;
    testSigning: boolean;
    rdpHardwareGpu: boolean;
    rebootRequired: boolean;
    spiceHelpers?: boolean; // the SPICE agent and the resolution helper are installed
    autoLogon?: boolean; // Windows signs in by itself (needed by the SPICE display route)
};

function readTrimmed(file: string): string | null {
    try {
        return fs.readFileSync(file, "utf-8").trim();
    } catch {
        return null;
    }
}

function onPath(binary: string): boolean {
    const dirs = (process.env.PATH ?? "").split(":").concat(["/usr/bin", "/usr/local/bin", "/usr/sbin"]);
    return dirs.some(dir => dir && fs.existsSync(path.join(dir, binary)));
}

export function detectHostGpu(): HostGpuInfo {
    let names: string[] = [];
    try {
        names = fs.readdirSync(DRI_DEVICE).filter(name => /^renderD\d+$/.test(name));
    } catch {
        // No /dev/dri: no GPU, or no access to it
    }
    const renderNodes = names.sort().map(name => {
        const vendorId = readTrimmed(`/sys/class/drm/${name}/device/vendor`) ?? "";
        let driver = "";
        try {
            driver = path.basename(fs.readlinkSync(`/sys/class/drm/${name}/device/driver`));
        } catch {}
        return { node: path.join(DRI_DEVICE, name), vendorId, vendor: GPU_VENDORS[vendorId] ?? "GPU", driver };
    });

    const nvidiaPresent = renderNodes.some(node => node.vendorId === "0x10de");
    const cdiSpec = ["/etc/cdi", "/var/run/cdi"].some(dir =>
        ["nvidia.yaml", "nvidia.json"].some(file => fs.existsSync(path.join(dir, file))),
    );
    return {
        renderNodes,
        nvidia: {
            present: nvidiaPresent,
            containerToolkit: onPath("nvidia-container-cli") || onPath("nvidia-ctk"),
            cdiSpec,
            modeset: readTrimmed("/sys/module/nvidia_drm/parameters/modeset") === "Y",
        },
    };
}

/** Problems that keep the virtual GPU from working on this host, empty if none */
export function hostGpuProblems(host: HostGpuInfo, runtime: ContainerRuntimes, renderNode?: string): string[] {
    const problems: string[] = [];
    if (!host.renderNodes.length) {
        problems.push("No GPU render node found in /dev/dri");
        return problems;
    }
    const selected = renderNode ? host.renderNodes.find(node => node.node === renderNode) : undefined;
    const usesNvidia = selected ? selected.vendorId === "0x10de" : host.nvidia.present;
    if (usesNvidia) {
        if (!host.nvidia.containerToolkit) {
            problems.push("NVIDIA GPUs need the NVIDIA Container Toolkit (nvidia-container-toolkit)");
        } else if (runtime === ContainerRuntimes.PODMAN && !host.nvidia.cdiSpec) {
            problems.push(
                "Podman needs the toolkit's CDI specification: sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml",
            );
        }
        if (!host.nvidia.modeset) {
            problems.push("NVIDIA GPUs need the nvidia-drm.modeset=1 kernel parameter");
        }
    }
    return problems;
}

export function isVirtualGpuEnabled(compose: ComposeConfig): boolean {
    return compose.services.windows.environment.GPU === "Y" || displayRoute(compose) === DisplayRoute.SPICE;
}

export function virtualGpuRenderNode(compose: ComposeConfig): string | undefined {
    const env = compose.services.windows.environment;
    if (displayRoute(compose) === DisplayRoute.SPICE) {
        return (env.ARGUMENTS ?? "").match(/rendernode=(\S+)/)?.[1];
    }
    return env.RENDERNODE || undefined;
}

function imageVersion(image: string): number[] | null {
    const match = image.match(/:(\d+)\.(\d+)/);
    return match ? [parseInt(match[1]), parseInt(match[2])] : null;
}

/**
 * Turns the virtual GPU on or off in a compose config (a copy is returned). renderNode picks the GPU on hosts with
 * several (dockur picks one itself otherwise, for RDP). The image stays on 6.05+ when turning it off. The SPICE
 * route needs SPICE_IMAGE to exist (buildSpiceImage).
 */
export function withVirtualGpu(
    compose: ComposeConfig,
    enabled: boolean,
    runtime: ContainerRuntimes,
    host: HostGpuInfo,
    renderNode?: string,
    route: DisplayRoute = DisplayRoute.RDP,
): ComposeConfig {
    const result: ComposeConfig = structuredClone(compose);
    const service = result.services.windows;
    const env = service.environment;

    // Start from a clean slate, so turning it on twice or switching GPUs or routes leaves nothing stale
    delete env.GPU;
    delete env.RENDERNODE;
    delete env.GBM_BACKENDS_PATH;
    delete env.NVIDIA_DRIVER_CAPABILITIES;
    if (env.DISPLAY === "none") delete env.DISPLAY;
    if (env.ARGUMENTS) env.ARGUMENTS = withoutSpiceArguments(env.ARGUMENTS);
    service.volumes = service.volumes.filter(volume => volume !== SPICE_VOLUME);
    if (service.image.startsWith(SPICE_IMAGE_NAME)) service.image = VIRTUAL_GPU_IMAGE;
    service.devices = service.devices.filter(device => device !== DRI_DEVICE && device !== NVIDIA_CDI_DEVICE);
    delete service.deploy;
    if (!enabled) {
        return result;
    }

    const version = imageVersion(service.image);
    if (
        !version ||
        version[0] < VIRTUAL_GPU_MIN_VERSION[0] ||
        (version[0] === VIRTUAL_GPU_MIN_VERSION[0] && version[1] < VIRTUAL_GPU_MIN_VERSION[1])
    ) {
        service.image = VIRTUAL_GPU_IMAGE;
    }
    if (route === DisplayRoute.SPICE) {
        service.image = SPICE_IMAGE;
        env.DISPLAY = "none";
        const node = renderNode || host.renderNodes[0]?.node || "/dev/dri/renderD128";
        env.ARGUMENTS = [env.ARGUMENTS, spiceArguments(node)].filter(Boolean).join(" ");
        fs.mkdirSync(SPICE_HOST_DIR, { recursive: true });
        service.volumes.push(SPICE_VOLUME);
    } else {
        env.GPU = "Y";
        if (renderNode) {
            env.RENDERNODE = renderNode;
        }
    }
    service.devices.push(DRI_DEVICE);

    const selected = renderNode ? host.renderNodes.find(node => node.node === renderNode) : undefined;
    const usesNvidia = selected ? selected.vendorId === "0x10de" : host.nvidia.present;
    if (usesNvidia) {
        env.GBM_BACKENDS_PATH = NVIDIA_GBM_BACKENDS_PATH;
        env.NVIDIA_DRIVER_CAPABILITIES = "all";
        if (runtime === ContainerRuntimes.PODMAN) {
            service.devices.push(NVIDIA_CDI_DEVICE);
        } else {
            service.deploy = {
                resources: {
                    reservations: {
                        devices: [
                            { driver: "nvidia", count: "all", capabilities: ["gpu", "graphics", "display", "utility"] },
                        ],
                    },
                },
            };
        }
    }
    return result;
}
