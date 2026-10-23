<template>
    <x-card
        class="flex relative flex-row justify-between items-center p-2 py-3 my-0 w-full backdrop-blur-xl backdrop-brightness-150 bg-neutral-800/20"
    >
        <div class="w-full">
            <div class="flex flex-row gap-2 items-center mb-2">
                <Icon class="inline-flex text-violet-400 size-8" icon="bi:gpu-card"></Icon>
                <h1 class="my-0 text-lg font-semibold">
                    Virtual GPU (3D acceleration)
                    <span class="bg-violet-500 rounded-full px-3 py-0.5 text-sm ml-2"> Experimental </span>
                </h1>
            </div>
            <x-label class="text-neutral-400 text-[0.9rem] !pt-0 !mt-0">
                Windows apps render on your GPU (NVIDIA, AMD or Intel) through a virtual GPU, while Linux keeps using
                it: OpenGL 4.2, Direct3D 9 and Direct3D 10/11 (feature level 10_0). No Vulkan or Direct3D 12 yet. The
                Windows driver is test-signed, so this turns on Windows test signing in the VM.
            </x-label>

            <x-card
                v-for="(problem, k) of problems"
                :key="`vgpu-problem-${k}`"
                class="flex items-center py-2 w-full my-2 backdrop-blur-xl gap-4 backdrop-brightness-150 bg-yellow-200/10"
            >
                <Icon class="inline-flex shrink-0 text-yellow-500 size-8" icon="clarity:warning-solid"></Icon>
                <p class="my-0 text-base font-normal text-yellow-200">{{ problem }}</p>
            </x-card>

            <div v-if="host.renderNodes.length" class="flex flex-row gap-4 items-center mt-3">
                <x-label class="text-neutral-300 shrink-0">GPU</x-label>
                <x-select
                    class="w-full"
                    :disabled="enabled || isBusy"
                    @change="(e: any) => (renderNode = e.detail.newValue === 'auto' ? '' : e.detail.newValue)"
                >
                    <x-menu>
                        <x-menuitem value="auto" :toggled="!renderNode">
                            <x-label>Automatic</x-label>
                        </x-menuitem>
                        <x-menuitem
                            v-for="node of host.renderNodes"
                            :key="node.node"
                            :value="node.node"
                            :toggled="renderNode === node.node"
                        >
                            <x-label
                                >{{ node.vendor }} ({{ node.driver || "unknown driver" }}, {{ node.node }})</x-label
                            >
                        </x-menuitem>
                    </x-menu>
                </x-select>
            </div>

            <div class="flex flex-row gap-4 items-center mt-3">
                <x-label class="text-neutral-300 shrink-0">Display</x-label>
                <x-select class="w-full" :disabled="isBusy" @change="(e: any) => (route = e.detail.newValue)">
                    <x-menu>
                        <x-menuitem :value="DisplayRoute.RDP" :toggled="route === DisplayRoute.RDP">
                            <x-label>App windows (RDP, up to 30 fps)</x-label>
                        </x-menuitem>
                        <x-menuitem
                            :value="DisplayRoute.SPICE"
                            :toggled="route === DisplayRoute.SPICE"
                            :disabled="!viewerBundled"
                        >
                            <x-label>Low-latency (SPICE, at your monitor's refresh rate)</x-label>
                        </x-menuitem>
                    </x-menu>
                </x-select>
                <x-button
                    v-if="enabled && (route !== appliedRoute || spiceOutdated || spiceNeedsAutoLogon)"
                    :disabled="isBusy"
                    class="shrink-0 !bg-gradient-to-tl from-blue-400/20 to-transparent hover:from-blue-400/30 transition"
                    @click="applyRoute"
                >
                    <x-label>Apply (restarts Windows)</x-label>
                </x-button>
            </div>
            <p v-if="route === DisplayRoute.SPICE" class="mt-2 mb-0 text-sm text-neutral-400">
                Straight from the GPU without compression, at your monitor's refresh rate. Apps open in their own
                windows; "Open desktop" shows the whole Windows desktop. The first switch builds the VM image on this
                computer (several minutes).
            </p>
            <p
                v-if="enabled && spiceOutdated && route === appliedRoute"
                class="mt-2 mb-0 text-sm text-amber-300"
            >
                Set up by an older WinBoat: Apply updates it (apps in their own windows need this).
            </p>
            <p v-if="!viewerBundled" class="mt-2 mb-0 text-sm text-neutral-400">
                This WinBoat build doesn't include the SPICE viewer (spice/build-viewer.sh).
            </p>
            <div v-if="enabled && appliedRoute === DisplayRoute.SPICE" class="flex flex-row gap-2 items-center mt-3">
                <x-label class="text-sm text-neutral-300 shrink-0">Behind app windows</x-label>
                <x-select
                    class="w-56"
                    @change="(e: any) => (wbConfig.config.seamlessBackground = e.detail.newValue)"
                >
                    <x-menu>
                        <x-menuitem value="black" :toggled="wbConfig.config.seamlessBackground === 'black'">
                            <x-label>Black</x-label>
                        </x-menuitem>
                        <x-menuitem
                            value="transparent"
                            :toggled="wbConfig.config.seamlessBackground === 'transparent'"
                        >
                            <x-label>Transparent (Linux desktop)</x-label>
                        </x-menuitem>
                    </x-menu>
                </x-select>
            </div>
            <div v-if="enabled && appliedRoute === DisplayRoute.SPICE" class="flex flex-row gap-2 items-center mt-3">
                <x-label class="text-sm text-neutral-300 shrink-0">Windows refresh rate</x-label>
                <x-select class="w-56" @change="(e: any) => setRefreshRate(parseInt(e.detail.newValue))">
                    <x-menu>
                        <x-menuitem
                            v-for="rate in refreshRates"
                            :key="rate"
                            :value="String(rate)"
                            :toggled="wbConfig.config.vmRefreshRate === rate"
                        >
                            <x-label>{{ rate === 0 ? "Match monitor" : `${rate} Hz` }}</x-label>
                        </x-menuitem>
                    </x-menu>
                </x-select>
            </div>
            <p v-if="enabled && appliedRoute === DisplayRoute.SPICE" class="mt-2 mb-0 text-sm text-neutral-400">
                How many frames a second Windows draws. The virtual GPU's work shares a thread with the VM's sound card:
                at high refresh rates on large monitors, busy apps can make the sound crackle. 60 Hz keeps it smooth
                and uses less CPU; Match monitor follows the monitor the viewer is on. Applies right away.
            </p>
            <p v-if="enabled && appliedRoute === DisplayRoute.SPICE" class="mt-2 mb-0 text-sm text-neutral-400">
                Where an app window isn't fully opaque (rounded corners, see-through windows), Windows' desktop shows.
                Black hides your Windows wallpaper while app windows are shown; Transparent shows the Linux desktop
                there instead. Applies when the app windows open next.
            </p>
            <x-button
                v-if="enabled && appliedRoute === DisplayRoute.SPICE"
                :disabled="isBusy"
                class="mt-3 !bg-gradient-to-tl from-violet-400/20 to-transparent hover:from-violet-400/30 transition"
                @click="openDesktop"
            >
                <x-label>Open desktop</x-label>
            </x-button>

            <x-card v-if="enabled" class="flex flex-col gap-1 py-2 w-full mt-4 bg-white/5">
                <p class="my-0 text-base text-gray-200">Windows driver</p>
                <template v-if="guestStatus">
                    <p class="my-0 text-sm text-neutral-400">
                        <template v-if="guestStatus.installed.present">
                            Installed: {{ guestStatus.installed.version }}
                            <template v-if="guestStatus.device"> · {{ guestStatus.device.name }}</template>
                        </template>
                        <template v-else>Not installed</template>
                        <template v-if="guestStatus.rebootRequired"> · restart Windows to finish</template>
                    </p>
                    <p v-if="!guestStatus.bundled.present" class="my-0 text-sm text-red-400">
                        ❗ This WinBoat build doesn't include the driver
                    </p>
                    <p v-else-if="needsDriverUpdate" class="my-0 text-sm text-yellow-200">
                        This WinBoat includes driver {{ guestStatus.bundled.version }}
                    </p>
                </template>
                <p v-else-if="!winboat.isOnline.value" class="my-0 text-sm text-neutral-400">
                    Start Windows to see the driver's state
                </p>
                <x-button
                    v-if="guestStatus && guestStatus.bundled.present && needsDriverUpdate"
                    :disabled="isBusy"
                    class="mt-2 self-start !bg-gradient-to-tl from-blue-400/20 to-transparent hover:from-blue-400/30 transition"
                    @click="installDriver"
                >
                    <x-label
                        >{{ guestStatus.installed.present ? "Update driver" : "Install driver" }} (restarts
                        Windows)</x-label
                    >
                </x-button>
            </x-card>

            <p v-if="step" class="mt-3 mb-0 text-neutral-300">⏳ {{ step }}</p>
            <p v-if="buildLine" class="mt-1 mb-0 text-xs font-mono text-neutral-500 truncate">{{ buildLine }}</p>
            <p v-if="errorMessage" class="mt-2 mb-0 text-red-500">❗ {{ errorMessage }}</p>
        </div>
        <x-button
            :disabled="isBusy || (!enabled && problems.length > 0)"
            class="shrink-0 ml-4 transition !border-0"
            :class="
                enabled
                    ? '!bg-gradient-to-tl from-red-500/20 to-transparent hover:from-red-500/30'
                    : '!bg-gradient-to-tl from-blue-400/20 to-transparent hover:from-blue-400/30'
            "
            @click="enabled ? disable() : enable()"
        >
            <x-throbber v-if="isBusy" class="w-8"></x-throbber>
            <x-label v-else>{{ enabled ? "Turn off" : "Turn on" }}</x-label>
        </x-button>
    </x-card>
</template>

<script setup lang="ts">
import { Icon } from "@iconify/vue";
import { computed, onMounted, ref, watch } from "vue";
import { WinboatConfig } from "../lib/config";
import {
    buildSpiceImage,
    detectHostGpu,
    displayRoute,
    DisplayRoute,
    hostGpuProblems,
    isVirtualGpuEnabled,
    openSpiceViewer,
    spiceRouteOutdated,
    spiceImageExists,
    spiceViewerBundled,
    virtualGpuRenderNode,
    withVirtualGpu,
    type GuestGpuStatus,
    type HostGpuInfo,
} from "../lib/virtualGpu";
import { logger, Winboat } from "../lib/winboat";

const emit = defineEmits<{ applied: [] }>();

const winboat = Winboat.getInstance();
const wbConfig = WinboatConfig.getInstance();

const host = ref<HostGpuInfo>(detectHostGpu());
const enabled = ref(false);
const renderNode = ref("");
const guestStatus = ref<GuestGpuStatus | null>(null);
const step = ref("");
const errorMessage = ref("");
const isBusy = ref(false);
const route = ref<DisplayRoute>(DisplayRoute.RDP); // selected
const appliedRoute = ref<DisplayRoute>(DisplayRoute.RDP); // in the compose file
const buildLine = ref("");
const spiceOutdated = ref(false); // set up by an older WinBoat: Apply updates it
// The SPICE route shows the console session, which Windows has to sign in to by itself
const spiceNeedsAutoLogon = computed(
    () => appliedRoute.value === DisplayRoute.SPICE && guestStatus.value?.autoLogon === false,
);
const viewerBundled = spiceViewerBundled();

const problems = computed(() =>
    hostGpuProblems(host.value, wbConfig.config.containerRuntime, renderNode.value || undefined),
);
const needsDriverUpdate = computed(
    () =>
        !!guestStatus.value &&
        (!guestStatus.value.installed.present ||
            guestStatus.value.installed.fingerprint !== guestStatus.value.bundled.fingerprint),
);

function readCompose() {
    return Winboat.readCompose(winboat.containerMgr!.composeFilePath);
}

function loadComposeState() {
    const compose = readCompose();
    enabled.value = isVirtualGpuEnabled(compose);
    renderNode.value = virtualGpuRenderNode(compose) ?? "";
    appliedRoute.value = enabled.value ? displayRoute(compose) : DisplayRoute.RDP;
    spiceOutdated.value = enabled.value && spiceRouteOutdated(compose);
    route.value = appliedRoute.value;
}

/** The SPICE route's VM image, built on this computer the first time */
async function ensureSpiceImage() {
    const executable = winboat.containerMgr!.executableAlias;
    if (await spiceImageExists(executable)) return;
    step.value = "Building the SPICE VM image (first time only, several minutes)";
    try {
        await buildSpiceImage(executable, line => (buildLine.value = line));
    } finally {
        buildLine.value = "";
    }
}

// "Match monitor" (0) and the rates the driver offers
const refreshRates = [0, 60, 75, 90, 100, 120, 144, 165, 180, 240];

function setRefreshRate(rate: number) {
    wbConfig.config.vmRefreshRate = rate;
    if (winboat.isOnline.value) void winboat.applyRefreshRate();
}

async function openDesktop() {
    errorMessage.value = "";
    try {
        await openSpiceViewer(
            winboat.containerMgr!.executableAlias,
            winboat.containerMgr!.containerName,
            undefined,
            undefined,
            rate => void winboat.matchRefreshRate(rate),
        );
    } catch (e) {
        logger.error("Virtual GPU: opening the SPICE desktop failed");
        logger.error(e);
        errorMessage.value = `${e instanceof Error ? e.message : e}`;
    }
}

async function applyRoute() {
    isBusy.value = true;
    errorMessage.value = "";
    try {
        if (!winboat.isOnline.value)
            throw new Error("Start Windows first: the display settings are applied from inside it");
        if (route.value === DisplayRoute.SPICE) {
            await ensureSpiceImage();
            // The SPICE route's GPU runs in a process of its own, which needs this WinBoat's driver (older ones
            // didn't negotiate 3D with it): it loads with the restart the switch brings
            step.value = "Waiting for the Guest Server update in Windows";
            const status = await waitForGuestGpuStatus();
            if (status.bundled.present && status.installed.fingerprint !== status.bundled.fingerprint) {
                step.value = "Updating the Windows driver";
                await winboat.setVirtualGpuDriver("install", false);
            }
        }
        // Windows restarts with the container: SPICE needs it to sign in by itself, RDP needs that off
        await winboat.setDesktopAutoLogon(route.value === DisplayRoute.SPICE);
        step.value = "Switching the display (Windows restarts)";
        const compose = withVirtualGpu(
            readCompose(),
            true,
            wbConfig.config.containerRuntime,
            host.value,
            renderNode.value || undefined,
            route.value,
        );
        await winboat.replaceCompose(compose);
        loadComposeState();
        emit("applied");
        step.value = "Waiting for Windows";
        await waitForGuest();
        if (appliedRoute.value === DisplayRoute.SPICE) await openDesktop();
    } catch (e) {
        logger.error("Virtual GPU: switching the display failed");
        logger.error(e);
        errorMessage.value = `${e instanceof Error ? e.message : e}`;
    } finally {
        step.value = "";
        isBusy.value = false;
    }
}

async function refreshGuestStatus() {
    if (!enabled.value || !winboat.isOnline.value) {
        guestStatus.value = null;
        return;
    }
    try {
        guestStatus.value = await winboat.getVirtualGpuStatus();
        // The SPICE route needs Windows to sign in by itself: set up by an older WinBoat, turn it on (from the
        // next start of Windows)
        if (appliedRoute.value === DisplayRoute.SPICE && guestStatus.value.autoLogon === false && !isBusy.value) {
            await winboat.setDesktopAutoLogon(true);
            guestStatus.value = await winboat.getVirtualGpuStatus();
        }
    } catch (e) {
        guestStatus.value = null;
        errorMessage.value = `${e instanceof Error ? e.message : e}`;
    }
}

/** Windows is up and the Guest Server matches this WinBoat (the update carries the driver) */
async function waitForGuest() {
    const deadline = Date.now() + 15 * 60_000;
    while (!winboat.isOnline.value || winboat.isUpdatingGuestServer.value) {
        if (Date.now() > deadline) throw new Error("Windows didn't come back up");
        await new Promise(resolve => setTimeout(resolve, 2000));
    }
}

/**
 * The Guest Server's virtual GPU state, once Windows is up and the Guest Server has been updated to this WinBoat's
 * (the online flag comes before the update starts, so an old Guest Server can answer first)
 */
async function waitForGuestGpuStatus(): Promise<GuestGpuStatus> {
    const deadline = Date.now() + 5 * 60_000;
    for (;;) {
        await waitForGuest();
        try {
            return await winboat.getVirtualGpuStatus();
        } catch (e) {
            if (Date.now() > deadline) throw e;
            await new Promise(resolve => setTimeout(resolve, 3000));
        }
    }
}

async function installDriver() {
    isBusy.value = true;
    errorMessage.value = "";
    try {
        step.value = "Installing the Windows driver";
        await winboat.setVirtualGpuDriver("install", true);
        step.value = "Restarting Windows to load the driver";
        if (!(await winboat.waitForGuestRestart())) throw new Error("Windows didn't come back up after the restart");
        await refreshGuestStatus();
    } catch (e) {
        logger.error("Virtual GPU: installing the driver failed");
        logger.error(e);
        errorMessage.value = `${e instanceof Error ? e.message : e}`;
    } finally {
        step.value = "";
        isBusy.value = false;
    }
}

async function enable() {
    isBusy.value = true;
    errorMessage.value = "";
    try {
        // Windows has to be running with this WinBoat's Guest Server (which carries the driver) before the container
        // is touched: restarting Windows during a Guest Server update leaves it without one
        if (!winboat.isOnline.value) {
            throw new Error("Start Windows first: the driver is installed from inside it");
        }
        step.value = "Waiting for the Guest Server update in Windows";
        const before = await waitForGuestGpuStatus();
        if (!before.bundled.present) {
            throw new Error("This WinBoat build doesn't include the Windows driver");
        }

        if (route.value === DisplayRoute.SPICE) {
            await ensureSpiceImage();
            await winboat.setDesktopAutoLogon(true);
        }
        step.value = "Adding the virtual GPU to the container (Windows restarts)";
        const compose = withVirtualGpu(
            readCompose(),
            true,
            wbConfig.config.containerRuntime,
            host.value,
            renderNode.value || undefined,
            route.value,
        );
        await winboat.replaceCompose(compose);
        loadComposeState();
        emit("applied");

        step.value = "Waiting for Windows";
        guestStatus.value = await waitForGuestGpuStatus();
        if (!guestStatus.value.bundled.present) {
            throw new Error("This WinBoat build doesn't include the Windows driver");
        }
    } catch (e) {
        logger.error("Virtual GPU: turning it on failed");
        logger.error(e);
        errorMessage.value = `${e instanceof Error ? e.message : e}`;
        step.value = "";
        isBusy.value = false;
        return;
    }
    isBusy.value = false;
    await installDriver();
}

async function disable() {
    isBusy.value = true;
    errorMessage.value = "";
    try {
        // The driver comes out first, while Windows runs; recreating the container then restarts Windows
        if (winboat.isOnline.value) {
            await refreshGuestStatus();
            if (guestStatus.value?.installed.present) {
                step.value = "Removing the Windows driver";
                await winboat.setVirtualGpuDriver("uninstall", false);
            }
            if (appliedRoute.value === DisplayRoute.SPICE) {
                await winboat.setDesktopAutoLogon(false);
            }
        } else {
            throw new Error("Start Windows first, so the driver can be removed");
        }

        step.value = "Removing the virtual GPU from the container (Windows restarts)";
        const compose = withVirtualGpu(readCompose(), false, wbConfig.config.containerRuntime, host.value);
        await winboat.replaceCompose(compose);
        loadComposeState();
        guestStatus.value = null;
        emit("applied");
    } catch (e) {
        logger.error("Virtual GPU: turning it off failed");
        logger.error(e);
        errorMessage.value = `${e instanceof Error ? e.message : e}`;
    } finally {
        step.value = "";
        isBusy.value = false;
    }
}

onMounted(async () => {
    loadComposeState();
    await refreshGuestStatus();
});

watch(winboat.isOnline, () => {
    if (!isBusy.value) refreshGuestStatus();
});
</script>
