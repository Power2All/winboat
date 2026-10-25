import { assert } from "@vueuse/core";
import { ref, type Ref } from "vue";
import YAML from "yaml";
import type { ComposeConfig, CustomAppCallbacks, GuestServerVersion, Metrics, WinApp } from "../../types";
import { AppIcons } from "../data/appicons";
import { InternalApps } from "../data/internalapps";
import { getFreeRDP } from "../utils/getFreeRDP";
import { guestAuthHeaders, guestServerUpdateZipPath } from "../utils/guestServer";
import {
    DisplayRoute,
    displayRoute,
    openSpiceViewer,
    showSeamlessApp,
    waitForAppWindow,
    type GuestGpuStatus,
    isVirtualGpuEnabled,
    onViewerFps,
    virtualGpuRenderNode,
    hostGpuMemoryMB,
} from "./virtualGpu";
import { GpuMonitor, type GpuMetrics } from "./gpuMetrics";
import { setIntervalImmediately } from "../utils/interval";
import { createLogger } from "../utils/log";
import { openLink } from "../utils/openLink";
import { MultiMonitorMode, WinboatConfig } from "./config";
import { HOST_QMP_PORT, HOST_RDP_PORT, NOVNC_URL, WINBOAT_API_URL, WINBOAT_DIR, WINBOAT_UPDATE_URL } from "./constants";
import { ContainerRuntimes, createContainer } from "./containers/common";
import { ContainerManager, ContainerStatus, isStaleContainerError } from "./containers/container";
import { ExecFileAsyncError } from "./exec-helper";
import { QMPManager } from "./qmp";

const nodeFetch: typeof import("node-fetch").default = require("node-fetch");
const fs: typeof import("fs") = require("node:fs");
const path: typeof import("path") = require("node:path");
const { promisify }: typeof import("util") = require("node:util");
const { exec }: typeof import("child_process") = require("node:child_process");

const execAsync = promisify(exec);
const USAGE_PATH = path.join(WINBOAT_DIR, "appUsage.json");
export const logger = createLogger(path.join(WINBOAT_DIR, "winboat.log"));

enum CustomAppCommands {
    NOVNC_COMMAND = "NOVNC_COMMAND",
}

const presetApps: WinApp[] = [
    {
        Name: "⚙️ Windows Desktop",
        Icon: AppIcons[InternalApps.WINDOWS_DESKTOP],
        Source: "internal",
        Path: InternalApps.WINDOWS_DESKTOP,
        Args: "",
        Usage: 0,
    },
    {
        Name: "⚙️ Windows Explorer",
        Icon: AppIcons[InternalApps.WINDOWS_EXPLORER],
        Source: "internal",
        Path: "%windir%\\explorer.exe",
        Args: "",
        Usage: 0,
    },
    {
        Name: "⚙️ Windows Settings",
        Icon: AppIcons[InternalApps.WINDOWS_SETTINGS],
        Source: "internal",
        Path: "%windir%\\explorer.exe",
        Args: "ms-settings:",
        Usage: 0,
    },
    {
        Name: "🖥️ Browser Display",
        Icon: AppIcons[InternalApps.NOVNC_BROWSER],
        Source: "internal",
        Path: CustomAppCommands.NOVNC_COMMAND,
        Args: "",
        Usage: 0,
    },
];

/**
 * The stock RDP args that apply to all app launches by default
 */
const stockArgs = [
    "/cert:ignore",
    "+clipboard",
    "/sound:sys:pulse",
    "/microphone:sys:pulse",
    "/floatbar",
    "/compression",
    "/sec:tls",
];

/**
 * Returns second/original param if first is undefined or null, else first/test param
 */
const useOriginalIfUndefinedOrNull = (test: string | undefined, original: string) => {
    return test ?? original;
};

/**
 * For specifying custom behavior when launching an app (e.g. novnc)
 * Maps a {@link WinApp.Path} to a callback, which is called in {@link Winboat.launchApp} if specified
 */
const customAppCallbacks: CustomAppCallbacks = {
    [CustomAppCommands.NOVNC_COMMAND]: () => {
        openLink(NOVNC_URL);
    },
};

const QMP_WAIT_MS = 2000;
const FETCH_TIMEOUT = 1000;
const GUEST_ONLINE_TIMEOUT_MS = 60_000;
// Installing the virtual GPU driver signs and installs ~50 MB of files
const VIRTUAL_GPU_SCRIPT_TIMEOUT_MS = 5 * 60_000;

class AppManager {
    appCache: WinApp[] = [];
    appUsageCache: { [key: string]: number } = {};
    readonly #wbConfig: WinboatConfig | null = null;

    constructor() {
        if (!fs.existsSync(USAGE_PATH)) {
            fs.writeFileSync(USAGE_PATH, "{}");
        }

        this.#wbConfig = WinboatConfig.getInstance();
    }

    async updateAppCache(options: { forceRead?: boolean } = {}) {
        const res = await nodeFetch(`${WINBOAT_API_URL}/apps`, { headers: guestAuthHeaders() });
        const newApps = (await res.json()) as WinApp[];
        newApps.push(...presetApps, ...this.#wbConfig!.config.customApps);

        if (this.appCache.length == newApps.length && !options.forceRead) return;

        for (const appIdx in newApps) {
            newApps[appIdx].Usage = this.appCache.find(app => app.Name == newApps[appIdx].Name)?.Usage || 0;
            this.appUsageCache[newApps[appIdx].Name] = newApps[appIdx].Usage;
        }

        this.appCache = newApps;
    }

    async getApps(): Promise<WinApp[]> {
        if (this.appCache.length > 0) {
            return this.appCache;
        }

        // Get the usage object that's on the disk
        const fsUsage = Object.entries(JSON.parse(fs.readFileSync(USAGE_PATH, "utf-8"))) as any[];
        this.appCache = [];

        // Populate appCache with dummy WinApp object containing data from the disk
        for (const element of fsUsage) {
            this.appCache.push({
                ...presetApps[0],
                Name: element[0],
                Usage: element[1],
            });
        }

        await this.updateAppCache({ forceRead: true });

        const appCacheHumanReadable = this.appCache.map(obj => {
            const res = { ...obj } as any;
            delete res.Icon;
            return res;
        });

        logger.info(`AppCache: ${JSON.stringify(appCacheHumanReadable, null, 4)}`);

        return this.appCache;
    }

    incrementAppUsage(app: WinApp) {
        app.Usage!++;
        this.appUsageCache[app.Name]++;
    }

    async writeToDisk() {
        fs.writeFileSync(USAGE_PATH, JSON.stringify(this.appUsageCache));
    }

    /**
     * Adds a custom app to WinBoat's application list
     * @param name Name of the app
     * @param path Path of the app
     * @param args Args of the app
     * @param icon Icon of the app
     */
    async addCustomApp(name: string, path: string, args: string, icon: string) {
        const customWinApp: WinApp = {
            Name: name,
            Path: path,
            Args: args,
            Icon: icon,
            Source: "custom",
            Usage: 0,
        };
        this.appCache.push(customWinApp);
        this.appUsageCache[name] = 0;
        await this.writeToDisk();
        this.#wbConfig!.config.customApps = this.#wbConfig!.config.customApps.concat(customWinApp);
    }

    async updateCustomApp(oldName: string, updatedApp: { Name: string; Path: string; Args: string; Icon: string }) {
        this.appCache = this.appCache.map(app => (app.Name === oldName ? { ...app, ...updatedApp } : app));

        // update appUsage if name changed
        if (oldName !== updatedApp.Name) {
            this.appUsageCache[updatedApp.Name] = this.appUsageCache[oldName] ?? 0;
            delete this.appUsageCache[oldName];
        }

        // update persisted app config
        this.#wbConfig!.config.customApps = this.#wbConfig!.config.customApps.map(app =>
            app.Name == oldName ? { ...app, ...updatedApp } : app,
        );

        await this.writeToDisk();
    }

    /**
     * Removes a custom app from WinBoat's application list
     * @param app The app to remove
     */
    async removeCustomApp(app: WinApp) {
        this.appCache = this.appCache.filter(a => a.Name !== app.Name);
        this.appUsageCache = Object.fromEntries(Object.entries(this.appUsageCache).filter(([key]) => key !== app.Name));
        await this.writeToDisk();
        this.#wbConfig!.config.customApps = this.#wbConfig!.config.customApps.filter(a => a.Name !== app.Name);
    }
}

export class Winboat {
    private static instance: Winboat | null = null;
    // Update Intervals
    #healthInterval: NodeJS.Timeout | null = null;
    #metricsInverval: NodeJS.Timeout | null = null;
    #rdpConnectionStatusInterval: NodeJS.Timeout | null = null;
    #qmpInterval: NodeJS.Timeout | null = null;

    // Variables
    isOnline: Ref<boolean> = ref(false);
    /** The VM's use of the host GPU, measured while somebody watches it (watchGpu) */
    gpuMetrics: Ref<GpuMetrics> = ref<GpuMetrics>({
        state: "measuring",
        usage: 0,
        name: "",
        memoryUsed: null,
        hostUsage: null,
        commandsPerSecond: null,
    });
    /**
     * What goes through the virtual GPU: the driver's bytes per second (its counters, in the metrics) and the frames
     * per second the SPICE viewer gets (null: not known)
     */
    gpuTraffic: Ref<{ bytesPerSecond: number | null; framesPerSecond: number | null }> = ref({
        bytesPerSecond: null,
        framesPerSecond: null,
    });
    #lastGpuCounters: { bytes: number; at: number } | null = null;
    #gpuMonitor: GpuMonitor | null = null;
    #gpuWatchers = 0;
    /** Apps starting on the SPICE desktop: the app list shows them as starting until their windows show */
    startingApps: Ref<WinApp[]> = ref([]);
    isUpdatingGuestServer: Ref<boolean> = ref(false);
    containerStatus: Ref<ContainerStatus> = ref(ContainerStatus.EXITED);
    containerActionLoading: Ref<boolean> = ref(false);
    rdpConnected: Ref<boolean> = ref(false);
    metrics: Ref<Metrics> = ref<Metrics>({
        cpu: {
            usage: 0,
            frequency: 0,
        },
        ram: {
            used: 0,
            total: 0,
            percentage: 0,
        },
        disk: {
            used: 0,
            total: 0,
            percentage: 0,
        },
    });
    readonly #wbConfig: WinboatConfig | null = null;
    appMgr: AppManager | null = null;
    qmpMgr: QMPManager | null = null;
    containerMgr: ContainerManager | null = null;

    static getInstance() {
        Winboat.instance ??= new Winboat();
        return Winboat.instance;
    }

    private constructor() {
        this.#wbConfig = WinboatConfig.getInstance();
        this.containerMgr = createContainer(this.#wbConfig.config.containerRuntime);
        onViewerFps(fps => (this.gpuTraffic.value = { ...this.gpuTraffic.value, framesPerSecond: fps }));

        // This is a special interval which will never be destroyed
        setInterval(async () => {
            const _containerStatus = await this.containerMgr!.getStatus();

            if (_containerStatus !== this.containerStatus.value) {
                // ERROR is explicitly set, so don't overwrite it from periodic polling.
                // Keep it until the next user action.
                if (this.containerStatus.value !== ContainerStatus.ERROR) {
                    this.containerStatus.value = _containerStatus;
                    logger.info(`Winboat Container state changed to ${_containerStatus}`);

                    if (_containerStatus === ContainerStatus.RUNNING) {
                        await this.createAPIIntervals();
                    } else {
                        await this.destroyAPIIntervals();
                    }
                }
            }
        }, 1000);

        this.appMgr = new AppManager();
    }

    /**
     * Creates the intervals which rely on the Winboat Guest API.
     */
    async createAPIIntervals() {
        logger.info("Creating Winboat API intervals...");
        const HEALTH_WAIT_MS = 1000;
        const METRICS_WAIT_MS = 1000;
        const RDP_STATUS_WAIT_MS = 1000;

        // *** Health Interval ***
        // Make sure we don't have any existing intervals
        if (this.#healthInterval) {
            clearInterval(this.#healthInterval);
            this.#healthInterval = null;
        }

        this.#healthInterval = setInterval(async () => {
            const _isOnline = await this.getHealth();
            if (_isOnline !== this.isOnline.value) {
                this.isOnline.value = _isOnline;
                logger.info(`Winboat Guest API went ${this.isOnline.value ? "online" : "offline"}`);

                if (this.isOnline.value) {
                    await this.checkVersionAndUpdateGuestServer();
                } else {
                    this.#appliedTheme = null;
                }
            }
            if (this.isOnline.value && !this.isUpdatingGuestServer.value) this.#keepWindowsTheme();
        }, HEALTH_WAIT_MS);

        // *** Metrics Interval ***
        // Make sure we don't have any existing intervals
        if (this.#metricsInverval) {
            clearInterval(this.#metricsInverval);
            this.#metricsInverval = null;
        }

        this.#metricsInverval = setInterval(async () => {
            // If the guest is offline or updating, don't bother checking metrics
            if (!this.isOnline.value || this.isUpdatingGuestServer.value) return;
            this.metrics.value = await this.getMetrics();
            this.#updateGpuTraffic();
        }, METRICS_WAIT_MS);

        // *** RDP Connection Status Interval ***
        // Make sure we don't have any existing intervals
        if (this.#rdpConnectionStatusInterval) {
            clearInterval(this.#rdpConnectionStatusInterval);
            this.#rdpConnectionStatusInterval = null;
        }

        this.#rdpConnectionStatusInterval = setInterval(async () => {
            // If the guest is offline or updating, don't bother checking RDP status
            if (!this.isOnline.value || this.isUpdatingGuestServer.value) return;

            // If RDP monitoring is disabled, don't check status, just set it to false
            if (!this.#wbConfig?.config.rdpMonitoringEnabled) {
                this.rdpConnected.value = false;
                return;
            }

            // Check RDP status
            const _rdpConnected = await this.getRDPConnectedStatus();
            if (_rdpConnected !== this.rdpConnected.value) {
                this.rdpConnected.value = _rdpConnected;
                logger.info(`RDP connection status changed to ${_rdpConnected ? "connected" : "disconnected"}`);
            }
        }, RDP_STATUS_WAIT_MS);

        // *** QMP Interval ***
        // Make sure we don't have any existing intervals
        if (this.#qmpInterval) {
            clearInterval(this.#qmpInterval);
            this.#qmpInterval = null;
        }

        // TODO: Remove if statement once this feature gets rolled out.
        if (this.#wbConfig?.config.experimentalFeatures) {
            this.createQMPInterval();
        }
    }

    /**
     * Destroys the intervals which rely on the Winboat Guest API.
     * This is called when the container is in any state other than Running.
     */
    async destroyAPIIntervals() {
        logger.info("Destroying Winboat API intervals...");
        if (this.#healthInterval) {
            clearInterval(this.#healthInterval);
            this.#healthInterval = null;
            // Side-effect: Set isOnline to false
            this.isOnline.value = false;
        }

        if (this.#metricsInverval) {
            clearInterval(this.#metricsInverval);
            this.#metricsInverval = null;
        }

        if (this.#rdpConnectionStatusInterval) {
            clearInterval(this.#rdpConnectionStatusInterval);
            this.#rdpConnectionStatusInterval = null;
            // Side-effect: Set rdpConnected to false
            this.rdpConnected.value = false;
        }

        if (this.#qmpInterval) {
            clearInterval(this.#qmpInterval);
            this.#qmpInterval = null;

            // Side effect: We must destroy the QMP Manager
            try {
                if (this.qmpMgr && (await this.qmpMgr.isAlive())) {
                    this.qmpMgr.qmpSocket.destroy();
                }
                this.qmpMgr = null;
                logger.info("[destroyAPIIntervals] QMP Manager destroyed because container is no longer running");
            } catch (e) {
                logger.error("[destroyAPIIntervals] Failed to destroy QMP Manager");
                logger.error(e);
            }
        }
    }

    async getHealth() {
        // If /health returns 200, then the guest is ready
        try {
            const res = await nodeFetch(`${WINBOAT_API_URL}/health`, { signal: AbortSignal.timeout(FETCH_TIMEOUT) });
            return res.status === 200;
        } catch {
            return false;
        }
    }

    async getMetrics() {
        const res = await nodeFetch(`${WINBOAT_API_URL}/metrics`, {
            headers: guestAuthHeaders(),
            signal: AbortSignal.timeout(FETCH_TIMEOUT),
        });
        const metrics = (await res.json()) as Metrics;
        return metrics;
    }

    async getRDPConnectedStatus() {
        const res = await nodeFetch(`${WINBOAT_API_URL}/rdp/status`, {
            headers: guestAuthHeaders(),
            signal: AbortSignal.timeout(FETCH_TIMEOUT),
        });
        const status = (await res.json()) as { rdpConnected: boolean };
        return status.rdpConnected;
    }

    /** The virtual GPU driver's state in Windows (guest_server/scripts/gpu-driver.ps1) */
    async getVirtualGpuStatus(): Promise<GuestGpuStatus> {
        const res = await nodeFetch(`${WINBOAT_API_URL}/gpu`, {
            headers: guestAuthHeaders(),
            signal: AbortSignal.timeout(VIRTUAL_GPU_SCRIPT_TIMEOUT_MS),
        });
        if (res.status === 404) {
            throw new Error("The Guest Server in Windows is too old for the virtual GPU, restart WinBoat to update it");
        }
        const body = (await res.json()) as GuestGpuStatus & { error?: string };
        if (!res.ok || body.error) {
            throw new Error(body.error ?? `Guest Server returned ${res.status} for /gpu`);
        }
        return body;
    }

    /**
     * Installs or uninstalls the virtual GPU driver bundled with the Guest Server. Both need Windows to restart:
     * with reboot, the Guest Server restarts it right after answering.
     */
    /** Starts an app on the Windows desktop the SPICE display route shows (as the user signed in there) */
    async launchOnDesktop(app: WinApp): Promise<number> {
        const res = await nodeFetch(`${WINBOAT_API_URL}/desktop/launch`, {
            method: "POST",
            headers: { ...guestAuthHeaders(), "Content-Type": "application/json" },
            // Seamless: the window tracker reports its windows, which the viewer shows as windows of their own
            body: JSON.stringify({ path: app.Path, args: app.Args ?? "", seamless: true, taskbar: "hide" }),
            signal: AbortSignal.timeout(30_000),
        });
        if (!res.ok) {
            const error: Error & { status?: number } = new Error(
                (await res.text()).trim() || `Guest Server returned ${res.status} for /desktop/launch`,
            );
            error.status = res.status;
            throw error;
        }
        return ((await res.json()) as { pid: number }).pid;
    }

    /** Shows or hides Windows' taskbar on the desktop the SPICE display route shows */
    async setDesktopTaskbar(visible: boolean) {
        const res = await nodeFetch(`${WINBOAT_API_URL}/desktop/taskbar`, {
            method: "POST",
            headers: { ...guestAuthHeaders(), "Content-Type": "application/json" },
            body: JSON.stringify({ visible }),
            signal: AbortSignal.timeout(30_000),
        });
        if (!res.ok) {
            throw new Error((await res.text()).trim() || `Guest Server returned ${res.status} for /desktop/taskbar`);
        }
    }

    /**
     * Sets Windows' refresh rate to the one of the monitor the SPICE window is on (the fastest Windows offers up to
     * it), so Windows composes as many frames as that monitor shows. A Guest Server without it, or Windows not
     * signed in yet, leaves the rate as it is.
     */
    #appliedTheme: "light" | "dark" | null = null;
    #themeTriedAt = 0;

    /** Windows' light or dark mode as the setting asks: the Linux desktop's, or a fixed one */
    wantedWindowsTheme(): "light" | "dark" {
        const setting = this.#wbConfig?.config.windowsTheme ?? "follow";
        if (setting !== "follow") return setting;
        const remote: typeof import("@electron/remote") = require("@electron/remote");
        return remote.nativeTheme.shouldUseDarkColors ? "dark" : "light";
    }

    /**
     * Keeps Windows in the wanted light or dark mode (the health check calls it): when Windows starts, when the
     * setting or the Linux desktop's mode changes. Until somebody is signed in to Windows it can't be set: tried again
     * every 15 seconds.
     */
    #keepWindowsTheme() {
        const mode = this.wantedWindowsTheme();
        if (mode === this.#appliedTheme || Date.now() - this.#themeTriedAt < 15_000) return;
        this.#themeTriedAt = Date.now();
        void this.setWindowsTheme(mode);
    }

    async setWindowsTheme(mode: "light" | "dark") {
        try {
            const res = await nodeFetch(`${WINBOAT_API_URL}/desktop/theme`, {
                method: "POST",
                headers: { ...guestAuthHeaders(), "Content-Type": "application/json" },
                body: JSON.stringify({ mode }),
                signal: AbortSignal.timeout(30_000),
            });
            if (!res.ok) throw new Error((await res.text()).trim() || `Guest Server returned ${res.status}`);
            this.#appliedTheme = mode;
            this.#themeTriedAt = 0;
            logger.info(`Windows is in ${mode} mode`);
        } catch (e) {
            logger.warn(`Setting Windows to ${mode} mode failed (tried again later): ${e}`);
        }
    }

    /** The refresh rate of the monitor the SPICE viewer is on, as it last reported */
    #monitorRate = 0;

    /**
     * Sets Windows' refresh rate for the SPICE route: the one the setting fixes, else the rate of the monitor the
     * viewer is on (it reports it when it starts). Windows composes that many frames a second, and the virtual GPU's
     * work runs on QEMU's main loop, which also feeds the sound card: high rates at large resolutions make the sound
     * crackle.
     */
    async matchRefreshRate(monitorRate: number) {
        this.#monitorRate = monitorRate;
        await this.applyRefreshRate();
    }

    /** Applies the refresh rate setting now (Windows has to be running) */
    async applyRefreshRate() {
        const fixed = this.#wbConfig?.config.vmRefreshRate ?? 0;
        const maxRate = fixed > 0 ? fixed : this.#monitorRate;
        if (maxRate <= 0) return;
        try {
            const res = await nodeFetch(`${WINBOAT_API_URL}/desktop/refresh-rate`, {
                method: "POST",
                headers: { ...guestAuthHeaders(), "Content-Type": "application/json" },
                body: JSON.stringify({ maxRate }),
                signal: AbortSignal.timeout(30_000),
            });
            if (!res.ok) throw new Error((await res.text()).trim() || `Guest Server returned ${res.status}`);
            const { rate } = (await res.json()) as { rate: number };
            logger.info(
                `SPICE display route: host monitor at ${this.#monitorRate || "?"} Hz, ` +
                    `${fixed > 0 ? `set to ${fixed} Hz` : "matching it"}, Windows at ${rate} Hz`,
            );
        } catch (e) {
            logger.error(`SPICE display route: setting Windows' refresh rate to ${maxRate} Hz failed`);
            logger.error(e);
        }
    }

    /**
     * Starts an app on the SPICE desktop and shows its windows as windows of their own. The app list shows it as
     * starting until its window shows (a minute at most): apps can take a while, and nothing showed meanwhile.
     */
    async startSeamless(app: WinApp) {
        const isApp = (other: WinApp) => other.Path === app.Path && other.Args === app.Args;
        const waiting = waitForAppWindow(60_000);
        this.startingApps.value = [...this.startingApps.value, app];
        void waiting.shown.then(() => (this.startingApps.value = this.startingApps.value.filter(a => !isApp(a))));
        try {
            await this.showSeamless(await this.launchOnDesktop(app));
        } catch (e) {
            waiting.cancel();
            throw e;
        }
    }

    /**
     * Measures the VM's use of the host GPU into gpuMetrics until the returned function is called (the Home view
     * shows it): measuring runs helper processes, only while it shows. Without the virtual GPU it's "off".
     */
    watchGpu(): () => void {
        if (this.#gpuWatchers++ === 0) {
            const compose = Winboat.readCompose(this.containerMgr!.composeFilePath);
            if (!isVirtualGpuEnabled(compose)) {
                this.gpuMetrics.value = { ...this.gpuMetrics.value, state: "off", usage: 0 };
            } else {
                this.gpuMetrics.value = { ...this.gpuMetrics.value, state: "measuring", usage: 0 };
                this.#gpuMonitor = new GpuMonitor(
                    this.containerMgr!.executableAlias,
                    this.containerMgr!.containerName,
                    virtualGpuRenderNode(compose),
                    metrics => (this.gpuMetrics.value = metrics),
                );
            }
        }
        let watching = true;
        return () => {
            if (!watching) return;
            watching = false;
            if (--this.#gpuWatchers === 0) {
                this.#gpuMonitor?.stop();
                this.#gpuMonitor = null;
            }
        };
    }

    /** The driver's bytes per second, from its totals in two metrics samples */
    #updateGpuTraffic() {
        const counters = this.metrics.value.gpu;
        if (!counters) {
            this.#lastGpuCounters = null;
            this.gpuTraffic.value = { ...this.gpuTraffic.value, bytesPerSecond: null };
            return;
        }
        const sample = {
            bytes: counters.commandBytes + counters.uploadBytes + counters.downloadBytes,
            at: Date.now(),
        };
        const last = this.#lastGpuCounters;
        // A driver that started again counts from 0
        if (last && sample.at > last.at && sample.bytes >= last.bytes) {
            const bytesPerSecond = ((sample.bytes - last.bytes) * 1000) / (sample.at - last.at);
            this.gpuTraffic.value = { ...this.gpuTraffic.value, bytesPerSecond };
        }
        this.#lastGpuCounters = sample;
    }

    /** Whether an app is starting on the SPICE desktop (its window hasn't shown yet) */
    isStarting(app: WinApp) {
        return this.startingApps.value.some(other => other.Path === app.Path && other.Args === app.Args);
    }

    /** Shows the windows of an app started on the SPICE desktop as windows of their own */
    async showSeamless(pid: number) {
        await showSeamlessApp(
            this.containerMgr!.executableAlias,
            this.containerMgr!.containerName,
            pid,
            this.#wbConfig?.config.seamlessBackground ?? "black",
            // The Multi-Monitor setting: Windows' desktop spans all monitors
            [MultiMonitorMode.MultiMon, MultiMonitorMode.Span].includes(this.#wbConfig?.config.multiMonitor!),
            rate => void this.matchRefreshRate(rate),
        );
    }

    /**
     * Starts the app once someone signs in to the Windows desktop (in the SPICE window), trying for a few minutes.
     * Auto logon signs in by itself from Windows' next start.
     */
    async launchWhenSignedIn(app: WinApp) {
        const deadline = Date.now() + 5 * 60_000;
        while (Date.now() < deadline) {
            await new Promise(resolve => setTimeout(resolve, 3000));
            try {
                await this.showSeamless(await this.launchOnDesktop(app));
                logger.info(`SPICE display route: started '${app.Name}' after signing in`);
                return;
            } catch (e) {
                if ((e as { status?: number }).status !== 409) {
                    logger.error(e);
                    return;
                }
            }
        }
        logger.warn(`SPICE display route: nobody signed in, '${app.Name}' wasn't started`);
    }

    /** Windows' auto logon, which the SPICE display route needs (WinBoat's installer turns it off for RDP) */
    async setDesktopAutoLogon(enable: boolean) {
        logger.info(`SPICE display route: auto logon ${enable ? "on" : "off"}`);
        const { username, password } = this.getCredentials();
        const res = await nodeFetch(`${WINBOAT_API_URL}/desktop/autologon`, {
            method: "POST",
            headers: { ...guestAuthHeaders(), "Content-Type": "application/json" },
            body: JSON.stringify({ enable, username, password }),
            signal: AbortSignal.timeout(30_000),
        });
        if (!res.ok) {
            throw new Error((await res.text()).trim() || `Guest Server returned ${res.status} for /desktop/autologon`);
        }
    }

    async setVirtualGpuDriver(action: "install" | "uninstall", reboot: boolean): Promise<GuestGpuStatus> {
        // The host GPU's video memory, which the driver reports and allows (older guest servers ignore it)
        const memoryMB =
            action === "install"
                ? hostGpuMemoryMB(virtualGpuRenderNode(Winboat.readCompose(this.containerMgr!.composeFilePath)))
                : 0;
        logger.info(
            `Virtual GPU: ${action} the driver in Windows${reboot ? " and restart it" : ""}` +
                (memoryMB ? ` (video memory: ${memoryMB} MB, the host GPU's)` : ""),
        );
        const res = await nodeFetch(`${WINBOAT_API_URL}/gpu/driver`, {
            method: "POST",
            headers: { ...guestAuthHeaders(), "Content-Type": "application/json" },
            body: JSON.stringify({ action, reboot, memoryMB }),
            signal: AbortSignal.timeout(VIRTUAL_GPU_SCRIPT_TIMEOUT_MS),
        });
        const body = (await res.json()) as GuestGpuStatus & { error?: string };
        if (!res.ok || body.error) {
            throw new Error(body.error ?? `Guest Server returned ${res.status} for /gpu/driver`);
        }
        return body;
    }

    /** Waits for Windows to go down (a restart the guest scheduled) and for the Guest Server to answer again */
    async waitForGuestRestart(timeoutMs = 10 * 60_000): Promise<boolean> {
        const deadline = Date.now() + timeoutMs;
        const offlineDeadline = Date.now() + 90_000;
        while ((await this.getHealth()) && Date.now() < offlineDeadline) {
            await new Promise(resolve => setTimeout(resolve, 2000));
        }
        while (Date.now() < deadline) {
            if (await this.getHealth()) return true;
            await new Promise(resolve => setTimeout(resolve, 3000));
        }
        return false;
    }

    static readCompose(composePath: string): ComposeConfig {
        const composeFile = fs.readFileSync(composePath, "utf-8");
        const composeContents = YAML.parse(composeFile) as ComposeConfig;
        return composeContents;
    }

    getCredentials() {
        const compose = Winboat.readCompose(this.containerMgr!.composeFilePath);
        return {
            username: compose.services.windows.environment.USERNAME,
            password: compose.services.windows.environment.PASSWORD,
        };
    }

    async #connectQMPManager() {
        try {
            this.qmpMgr = await QMPManager.createConnection("127.0.0.1", HOST_QMP_PORT).catch(e => {
                logger.error(e);
                throw e;
            });
            const capabilities = await this.qmpMgr.executeCommand("qmp_capabilities");
            assert("return" in capabilities);

            const commands = await this.qmpMgr.executeCommand("query-commands");
            // @ts-ignore property "result" already exists due to assert
            assert(commands.return.every(x => "name" in x));
        } catch (e) {
            logger.error("There was an error connecting to QMP");
            logger.error(e);
        }
    }

    createQMPInterval() {
        logger.info("[createQMPInterval] Creating new QMP Interval");
        this.#qmpInterval = setIntervalImmediately(async () => {
            if (!this.#wbConfig?.config.experimentalFeatures) {
                clearInterval(this.#qmpInterval!);
                this.#qmpInterval = null;
                logger.info("[QMPInterval] Destroying self because experimentalFeatures was turned off");
            }

            // If QMP already exists and healthy, we're good
            if (this.qmpMgr && (await this.qmpMgr.isAlive())) return;

            // Otherwise, connect to it since the container is alive but
            // QMP either doesn't exist or is disconnected
            await this.#connectQMPManager();
            logger.info("[QMPInterval] Created new QMP Manager");
        }, QMP_WAIT_MS);
    }

    /**
     * Recreates the WinBoat container from its existing compose file.
     * This is used to recover from a stale/malfunctioning container, e.g. one
     * that references a passed-through USB device that no longer exists on
     * the host, which prevents it from being started normally.
     * @note Mirrors the manual workaround of `container rm` + `compose up`.
     */
    async recreateContainer() {
        logger.warn("[recreateContainer] Recreating WinBoat container from its compose file...");
        await this.containerMgr!.remove();
        await this.containerMgr!.compose("up");
        logger.info("[recreateContainer] Successfully recreated WinBoat container");
    }

    async startContainer() {
        logger.info("Starting WinBoat container...");
        this.containerActionLoading.value = true;

        try {
            // Start the container if it exists and recreate it if starting it runs into an error
            // If there is no container, create it from the compose file
            if (await this.containerMgr!.exists()) {
                try {
                    await this.containerMgr!.container("start");
                } catch (e) {
                    if (isStaleContainerError(e)) {
                        logger.warn(
                            "[startContainer] Container appears to be stale/malfunctioning (e.g. a stale USB passthrough reference). Attempting to recreate it...",
                        );
                        await this.recreateContainer();
                    } else {
                        throw e;
                    }
                }
                logger.info("Successfully started WinBoat container");
            } else {
                try {
                    await this.containerMgr!.compose("up");
                    const recreated = await this.containerMgr!.exists();

                    if (recreated) {
                        logger.info("Successfully recreated the WinBoat container from the existing compose file");
                    } else {
                        logger.error(
                            "Failed to recreate the WinBoat container: it still doesn't exist after 'compose up'",
                        );
                    }
                } catch (e) {
                    logger.error("Failed to recreate the WinBoat container from the existing compose file");
                    logger.error(e);
                    throw e;
                }
            }
        } catch (e) {
            logger.error("There was an error performing the container action.");
            logger.error(e);
            this.containerStatus.value = ContainerStatus.ERROR;
            throw e;
        } finally {
            this.containerActionLoading.value = false;
        }
    }

    async stopContainer() {
        logger.info("Stopping WinBoat container...");
        this.containerActionLoading.value = true;
        try {
            await this.containerMgr!.container("stop");
            logger.info("Successfully stopped WinBoat container");
        } finally {
            this.containerActionLoading.value = false;
        }
    }

    async restartContainer() {
        logger.info("Restarting WinBoat container...");
        this.containerActionLoading.value = true;
        try {
            try {
                await this.containerMgr!.container("restart");
            } catch (e) {
                if (isStaleContainerError(e)) {
                    logger.warn(
                        "[restartContainer] Container appears to be stale/malfunctioning (e.g. a stale USB passthrough reference). Attempting to recreate it...",
                    );
                    await this.recreateContainer();
                } else {
                    throw e;
                }
            }
            logger.info("Successfully restarted WinBoat container");
        } catch (e) {
            logger.error("There was an error restarting the container.");
            logger.error(e);
            this.containerStatus.value = ContainerStatus.ERROR;
            throw e;
        } finally {
            this.containerActionLoading.value = false;
        }
    }

    async pauseContainer() {
        logger.info("Pausing WinBoat container...");
        this.containerActionLoading.value = true;
        try {
            await this.containerMgr!.container("pause");
            logger.info("Successfully paused WinBoat container");
        } finally {
            this.containerActionLoading.value = false;
        }
    }

    async unpauseContainer() {
        logger.info("Unpausing WinBoat container...");
        this.containerActionLoading.value = true;
        try {
            await this.containerMgr!.container("unpause");
            logger.info("Successfully unpaused WinBoat container");
        } finally {
            this.containerActionLoading.value = false;
        }
    }

    // TODO: refactor / possibly remove this
    /**
        Replaces the compose file, and and updates the container.
        @note Use {@link ContainerManager.writeCompose} in case only disk write is needed
    */
    async replaceCompose(composeConfig: ComposeConfig) {
        logger.info("Going to replace compose config");
        this.containerActionLoading.value = true;

        const composeFilePath = this.containerMgr!.composeFilePath;

        // 0. Stop the current container if it's online
        if (this.containerStatus.value === ContainerStatus.RUNNING) {
            await this.stopContainer();
        }

        // 1. Compose down the current container
        await this.containerMgr!.compose("down");

        // 2. Create a backup directory if it doesn't exist
        const backupDir = path.join(WINBOAT_DIR, "backup");

        if (!fs.existsSync(backupDir)) {
            fs.mkdirSync(backupDir);
            logger.info(`Created compose backup dir: ${backupDir}`);
        }

        // 3. Move the current compose file to backup
        const backupFile = `${Date.now()}-${path.basename(this.containerMgr!.composeFilePath)}`;
        fs.renameSync(composeFilePath, path.join(backupDir, backupFile));
        logger.info(`Backed up current compose at: ${path.join(backupDir, backupFile)}`);

        // 4. Write new compose file
        this.containerMgr!.writeCompose(composeConfig);
        logger.info(`Wrote new compose file to: ${composeFilePath}`);

        // 5. Deploy the container with the new compose file
        await this.containerMgr!.compose("up");

        logger.info("Replace compose config completed, successfully deployed new container");

        this.containerActionLoading.value = false;
    }

    async resetWinboat() {
        console.info("Resetting Winboat...");

        // 1. Stop container
        await this.stopContainer();
        console.info("Stopped container");

        // 2. Remove the container

        await this.containerMgr!.remove();
        console.info("Removed container");

        // 3. Remove the container volume or folder
        const compose = Winboat.readCompose(this.containerMgr!.composeFilePath);
        const storage = compose.services.windows.volumes.find(vol => vol.includes("/storage"));
        if (storage?.startsWith("data:")) {
            if (this.#wbConfig?.config.containerRuntime !== ContainerRuntimes.DOCKER) {
                logger.error("Volume not supported on podman runtime");
            }
            // In this case we have a volume (legacy)
            await execAsync("docker volume rm winboat_data");
            console.info("Removed volume");
        } else {
            const storageFolder = storage?.split(":").at(0) ?? null;
            if (storageFolder && fs.existsSync(storageFolder)) {
                fs.rmSync(storageFolder, { recursive: true, force: true });
                console.info(`Removed storage folder at ${storageFolder}`);
            } else {
                console.warn("Storage folder does not exist, skipping removal");
            }
        }

        // 4. Remove WinBoat directory
        fs.rmSync(WINBOAT_DIR, { recursive: true, force: true });
        console.info(`Removed ${WINBOAT_DIR}`);
        console.info("So long and thanks for all the fish!");
    }

    async launchApp(app: WinApp) {
        if (!this.isOnline.value) throw new Error("Cannot launch app, Winboat is offline");

        // The SPICE display route shows the whole desktop, and an RDP connection would take over the Windows session
        // it shows: open the desktop instead
        if (displayRoute(Winboat.readCompose(this.containerMgr!.composeFilePath)) === DisplayRoute.SPICE) {
            const internal = app.Path === InternalApps.WINDOWS_DESKTOP || !!customAppCallbacks[app.Path];
            logger.info(`SPICE display route: opening the desktop${internal ? "" : ` with '${app.Name}'`}`);
            const openDesktop = () =>
                openSpiceViewer(
                    this.containerMgr!.executableAlias,
                    this.containerMgr!.containerName,
                    "WinBoat",
                    "winboat-desktop",
                    rate => void this.matchRefreshRate(rate),
                );
            try {
                if (internal) {
                    await openDesktop();
                    await this.setDesktopTaskbar(true).catch(error => logger.error(error));
                } else {
                    if (this.isStarting(app)) {
                        // Clicked again while it starts: one is enough
                        logger.info(`SPICE display route: '${app.Name}' is starting already`);
                        return;
                    }
                    try {
                        // Seamless app windows: each of the app's windows in a Linux window of its own
                        await this.startSeamless(app);
                    } catch (e) {
                        if ((e as { status?: number }).status !== 409) throw e;
                        // Nobody is signed in to the desktop (auto logon was off at Windows' start): sign-in is
                        // automatic from the next start, and the app opens once someone signs in now (the desktop
                        // shows meanwhile)
                        logger.info("SPICE display route: Windows isn't signed in, waiting to start the app");
                        await openDesktop();
                        await this.setDesktopAutoLogon(true).catch(error => logger.error(error));
                        void this.launchWhenSignedIn(app);
                    }
                }
            } catch (e) {
                logger.error("SPICE display route: opening the desktop failed");
                logger.error(e);
                throw e;
            }
            this.appMgr?.incrementAppUsage(app);
            this.appMgr?.writeToDisk();
            return;
        }

        if (customAppCallbacks[app.Path]) {
            logger.info(`Found custom app command for '${app.Name}'`);
            customAppCallbacks[app.Path]!(this);
            this.appMgr?.incrementAppUsage(app);
            this.appMgr?.writeToDisk();
            return;
        }

        const cleanAppName = app.Name.replaceAll(/[,.'"]/g, "");
        const { username, password } = this.getCredentials();

        logger.info(`Launching app: ${app.Name} at path ${app.Path}`);

        const freeRDPInstallation = await getFreeRDP();

        // Arguments specified by user to override stock arguments
        const replacementArgs = this.#wbConfig?.config.rdpArgs.filter(a => a.isReplacement);
        // Additional (new) arguments added by user
        const newArgs = this.#wbConfig?.config.rdpArgs.filter(a => !a.isReplacement).map(v => v.newArg) ?? [];
        // The stock arguments after any replacements have been made and new arguments have been added
        const combinedArgs = stockArgs
            .map(argStr =>
                useOriginalIfUndefinedOrNull(replacementArgs?.find(r => argStr === r.original?.trim())?.newArg, argStr),
            )
            .concat(newArgs);
        let args = [`/u:${username}`, `/p:${password}`, `/v:127.0.0.1`, `/port:${HOST_RDP_PORT}`, ...combinedArgs];

        if (app.Path == InternalApps.WINDOWS_DESKTOP) {
            args = args.concat([
                "+f",
                this.#wbConfig?.config.smartcardEnabled ? "/smartcard" : "",
                `/scale:${this.#wbConfig?.config.scale ?? 100}`,
            ]);
        } else {
            args = args.concat([
                this.#wbConfig?.config.multiMonitor === MultiMonitorMode.Span ? "+span" : "",
                "-wallpaper",
                this.#wbConfig?.config.multiMonitor === MultiMonitorMode.MultiMon ? "/multimon" : "",
                `/scale-desktop:${this.#wbConfig?.config.scaleDesktop ?? 100}`,
                `/wm-class:winboat-${cleanAppName}`,
                // hidef goes before cmd: FreeRDP stops reading sub-options after the quoted cmd value
                `/app:program:${app.Path},name:${cleanAppName}${this.#wbConfig?.config.legacyRemoteApp ? ",hidef:off" : ""},cmd:"${app.Args}"`,
            ]);
        }

        args = args.filter((v, _i, _a) => v.trim() !== "");

        this.appMgr?.incrementAppUsage(app);
        this.appMgr?.writeToDisk();

        if (!freeRDPInstallation) {
            logger.error("No FreeRDP installation found");
            return;
        }

        try {
            const safeToLogArgs = freeRDPInstallation.stringifyExec(args).replace(/\/p:[^ ]+/g, "/p:********");
            logger.info(`Launch FreeRDP with command:\n${safeToLogArgs}`);
            await freeRDPInstallation.exec(args);
        } catch (e) {
            const execError = e as ExecFileAsyncError;
            const ERRINFO_RPC_INITIATED_DISCONNECT = 0x00000001;
            const ERRINFO_LOGOFF_BY_USER = 0x0000000c;

            // TODO: Handle all FreeRDP error codes
            // https://github.com/FreeRDP/FreeRDP/blob/3fc1c3ce31b5af1098d15603d7b3fe1c93cf77a5/include/freerdp/error.h#L58
            switch (execError.code) {
                case ERRINFO_RPC_INITIATED_DISCONNECT: {
                    logger.info("FreeRDP connection already established.");
                    logger.info("Creating new session..");
                    break;
                }
                case ERRINFO_LOGOFF_BY_USER: {
                    logger.info("FreeRDP disconnected due to user logging off.");
                    break;
                }
                default: {
                    logger.warn(`FreeRDP process returned error code '${execError.code}'`);
                }
            }
        }
    }

    async checkVersionAndUpdateGuestServer() {
        // 1. Compare the running Guest Server version with the bundled app version.
        const versionRes = await nodeFetch(`${WINBOAT_API_URL}/version`, { headers: guestAuthHeaders() });
        const version = (await versionRes.json()) as GuestServerVersion;
        const appVersion = import.meta.env.VITE_APP_VERSION;

        // Any mismatch triggers an update. This is intentionally not semver-aware:
        // installing an older WinBoat deliberately rolls the guest back to match.
        if (version.version === appVersion) {
            return;
        }
        logger.info(`Guest Server update needed: ${version.version} -> ${appVersion}`);

        // 2. Push the bundled update payload to the Guest Server Updater, which
        //    applies it atomically and rolls back if the new server fails to come
        //    up. Auth is the shared token; the raw zip is the request body.
        this.isUpdatingGuestServer.value = true;
        const zipPath = guestServerUpdateZipPath();
        logger.info(`Sending update payload to the Guest Server Updater: ${zipPath}`);

        try {
            const res = await nodeFetch(`${WINBOAT_UPDATE_URL}/update`, {
                method: "POST",
                headers: {
                    ...guestAuthHeaders(),
                    "Content-Type": "application/octet-stream",
                },
                body: fs.createReadStream(zipPath),
            });
            if (res.status !== 200) {
                throw new Error(`Updater returned ${res.status}: ${await res.text()}`);
            }
            logger.info("Guest Server Updater applied the update successfully");
        } catch (e) {
            logger.error("Failed to apply Guest Server update");
            logger.error(e);
            this.isUpdatingGuestServer.value = false;
            throw e;
        }

        // 3. The updater already health-gated the new server before responding;
        //    this just re-syncs the host's own online view, bounded by a
        //    wall-clock deadline (getHealth itself can block up to FETCH_TIMEOUT).
        let online = await this.getHealth();
        const deadline = Date.now() + GUEST_ONLINE_TIMEOUT_MS;
        while (!online && Date.now() < deadline) {
            await new Promise(resolve => setTimeout(resolve, 1000));
            online = await this.getHealth();
        }

        if (online) {
            logger.info("Update completed, Winboat Guest Server is online");
        } else {
            logger.error("Guest Server did not report healthy within the timeout after update");
        }

        this.isUpdatingGuestServer.value = false;
    }

    /**
     * Whether or not the Winboat singleton has a QMP interval active
     */
    get hasQMPInterval() {
        return this.#qmpInterval !== null;
    }
}
