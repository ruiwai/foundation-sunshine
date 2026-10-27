<script setup>
import { ref, computed, onBeforeUnmount, onMounted } from 'vue'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'
import VddPrerequisiteNotice from '../../components/common/VddPrerequisiteNotice.vue'
import AdapterNameSelector from './advanced/AdapterNameSelector.vue'
import CaptureCompatibilityOverrides from './advanced/CaptureCompatibilityOverrides.vue'
import { createNativeRtxHdrBridge } from '../../utils/nativeRtxHdrBridge.js'

const props = defineProps(['platform', 'config', 'global_prep_cmd'])

const config = ref(props.config)
const nativeRtxHdrManagerAvailable = ref(false)
let nativeRtxHdrBridge

const isVDDCaptureSelected = computed(() => {
  return props.platform === 'windows' && (config.value.capture === '' || config.value.capture === 'vdd')
})

onMounted(() => {
  nativeRtxHdrBridge = createNativeRtxHdrBridge({
    windowObject: window,
    documentObject: document,
    onAvailability: (available) => { nativeRtxHdrManagerAvailable.value = available },
  })
  window.addEventListener('message', nativeRtxHdrBridge.handleMessage)
  nativeRtxHdrBridge.requestContext()
})

onBeforeUnmount(() => {
  if (nativeRtxHdrBridge) {
    window.removeEventListener('message', nativeRtxHdrBridge.handleMessage)
  }
})

const openNativeRtxHdrManager = () => nativeRtxHdrBridge?.openManager()

// === 编解码器策略（HEVC + AV1 整合 UI） ===
// 底层仍写入 hevc_mode / av1_mode（保持 sunshine.conf 兼容），UI 层用一个策略 +
// 一个 HDR 复选框推算两者的值。
const showCodecAdvanced = ref(false)

// 把 config.value.hevc_mode / av1_mode 转成 number，便于比较（旧值可能是 string）
const hevcModeNum = computed(() => Number(config.value.hevc_mode ?? 0))
const av1ModeNum = computed(() => Number(config.value.av1_mode ?? 0))

const codecStrategy = computed({
  get() {
    const h = hevcModeNum.value
    const a = av1ModeNum.value
    if (h === 0 && a === 0) return 'auto'
    if (h === 1 && a === 1) return 'h264_only'
    // modern: 都通告（值为 2 或 3 都算 modern；HDR 由 enableHdr 单独决定）
    if ((h === 2 || h === 3) && (a === 2 || a === 3)) return 'modern'
    return 'custom'
  },
  set(v) {
    if (v === 'auto') {
      config.value.hevc_mode = 0
      config.value.av1_mode = 0
    } else if (v === 'h264_only') {
      config.value.hevc_mode = 1
      config.value.av1_mode = 1
    } else if (v === 'modern') {
      const hdr = enableHdr.value
      config.value.hevc_mode = hdr ? 3 : 2
      config.value.av1_mode = hdr ? 3 : 2
    }
    // 'custom' → 不修改值，由用户在展开区编辑
  },
})

// 当任意 codec 选择 mode 3（含 10-bit/HDR），即视为开启 HDR 通告
const enableHdr = computed({
  get() {
    return hevcModeNum.value === 3 || av1ModeNum.value === 3
  },
  set(v) {
    // 仅在"现代编码器"策略下生效
    if (codecStrategy.value !== 'modern') return
    config.value.hevc_mode = v ? 3 : 2
    config.value.av1_mode = v ? 3 : 2
  },
})

// HDR 复选框是否可用（只有 modern 策略下才有意义）
const hdrToggleDisabled = computed(() => codecStrategy.value !== 'modern')
</script>

<template>
  <div class="config-page">
    <!-- Codec Strategy (整合 HEVC + AV1) -->
    <div class="settings-panel settings-panel--accent">
      <label for="codec_strategy" class="form-label">{{ $t('config.codec_strategy') }}</label>
      <select id="codec_strategy" class="form-select" v-model="codecStrategy">
        <option value="auto">{{ $t('config.codec_strategy_auto') }}</option>
        <option value="modern">{{ $t('config.codec_strategy_modern') }}</option>
        <option value="h264_only">{{ $t('config.codec_strategy_h264') }}</option>
        <option value="custom" disabled v-if="codecStrategy !== 'custom'">
          {{ $t('config.codec_strategy_custom_locked') }}
        </option>
        <option value="custom" v-else>{{ $t('config.codec_strategy_custom') }}</option>
      </select>

      <div class="form-check mt-2">
        <input
          class="form-check-input"
          type="checkbox"
          id="codec_enable_hdr"
          v-model="enableHdr"
          :disabled="hdrToggleDisabled"
        />
        <label class="form-check-label" for="codec_enable_hdr">
          {{ $t('config.codec_enable_hdr') }}
        </label>
        <div class="form-text" v-if="hdrToggleDisabled">
          {{ $t('config.codec_enable_hdr_disabled_hint') }}
        </div>
      </div>

      <div class="form-text">{{ $t('config.codec_strategy_desc') }}</div>

      <!-- 偏离推荐值时给出温和提示 -->
      <div class="strategy-warning mt-2" v-if="codecStrategy !== 'auto'">
        <small>{{ $t('config.codec_strategy_non_default_warning') }}</small>
      </div>

      <!-- 高级（专家模式）：原 HEVC / AV1 dropdown -->
      <div class="mt-2">
        <button
          type="button"
          class="settings-disclosure"
          :aria-expanded="showCodecAdvanced"
          aria-controls="codec-advanced-panel"
          @click="showCodecAdvanced = !showCodecAdvanced"
        >
          {{ showCodecAdvanced ? $t('config.codec_advanced_hide') : $t('config.codec_advanced_show') }}
        </button>
      </div>

      <div v-if="showCodecAdvanced" id="codec-advanced-panel" class="settings-subpanel mt-3">
        <div class="mb-3">
          <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
          <select id="hevc_mode" class="form-select" v-model="config.hevc_mode">
            <option value="0">{{ $t('config.hevc_mode_0') }}</option>
            <option value="1">{{ $t('config.hevc_mode_1') }}</option>
            <option value="2">{{ $t('config.hevc_mode_2') }}</option>
            <option value="3">{{ $t('config.hevc_mode_3') }}</option>
          </select>
          <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
        </div>

        <div class="mb-0">
          <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
          <select id="av1_mode" class="form-select" v-model="config.av1_mode">
            <option value="0">{{ $t('config.av1_mode_0') }}</option>
            <option value="1">{{ $t('config.av1_mode_1') }}</option>
            <option value="2">{{ $t('config.av1_mode_2') }}</option>
            <option value="3">{{ $t('config.av1_mode_3') }}</option>
          </select>
          <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
        </div>
      </div>
    </div>

    <div class="settings-panel mt-3" v-if="platform === 'windows'">
      <div class="form-label">{{ $t('config.hdr_enhanced') }}</div>
      <div class="form-text">{{ $t('config.hdr_enhanced_desc') }}</div>
      <button
        type="button"
        class="btn btn-outline-primary btn-sm mt-2"
        :disabled="!nativeRtxHdrManagerAvailable"
        @click="openNativeRtxHdrManager"
      >
        <i class="fas fa-sliders-h me-1" aria-hidden="true"></i>
        {{ $t('config.hdr_enhanced_open_manager') }}
      </button>
      <div v-if="!nativeRtxHdrManagerAvailable" class="form-text">{{ $t('config.wgc_control_panel_only') }}</div>
    </div>

    <!-- Capture -->
    <div class="settings-panel mt-3" v-if="platform !== 'macos'">
      <AdapterNameSelector :platform="platform" :config="config" />

      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <select id="capture" class="form-select" v-model="config.capture">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #linux>
            <option value="nvfbc">NvFBC</option>
            <option value="wlr">wlroots</option>
            <option value="kms">KMS</option>
            <option value="x11">X11</option>
          </template>
          <template #windows>
            <option value="vdd">{{ $t('config.capture_vdd_direct') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">
        {{ $t('config.capture_desc') }}
        <span v-if="isVDDCaptureSelected" class="status-note is-info mt-2">
          <i class="fas fa-info-circle me-1"></i>
          {{ $t('config.capture_vdd_direct_desc') }}
        </span>
      </div>
      <VddPrerequisiteNotice :active="isVDDCaptureSelected" />
    </div>

    <!-- Encoder -->
    <div class="settings-field mt-3">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <select id="encoder" class="form-select" v-model="config.encoder">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #windows>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="quicksync">Intel QuickSync</option>
            <option value="amdvce">AMD AMF/VCE</option>
          </template>
          <template #linux>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="vaapi">VA-API</option>
          </template>
          <template #macos>
            <option value="videotoolbox">VideoToolbox</option>
          </template>
        </PlatformLayout>
        <option value="software">{{ $t('config.encoder_software') }}</option>
      </select>
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
    </div>

    <CaptureCompatibilityOverrides
      :platform="platform"
      :config="config"
    />
  </div>
</template>

<style scoped>
.strategy-warning,
.status-note {
  border: 1px solid color-mix(in srgb, var(--ui-warning) 34%, transparent);
  border-radius: var(--ui-radius-sm);
  background: color-mix(in srgb, var(--ui-warning) 11%, transparent);
  color: var(--ui-warning-text);
}

.strategy-warning {
  padding: 0.65rem 0.75rem;
}

.settings-disclosure {
  display: inline-flex;
  align-items: center;
  min-height: 2.1rem;
  padding: 0.35rem 0.75rem;
  border: 1px solid var(--ui-border-strong);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-surface);
  color: var(--ui-accent);
  font-size: 0.84rem;
  font-weight: 600;
  transition: background-color 0.2s ease, border-color 0.2s ease, box-shadow 0.2s ease;
}

.settings-disclosure:hover,
.settings-disclosure:focus-visible {
  border-color: var(--ui-accent);
  background: var(--ui-accent-soft);
  box-shadow: 0 0 0 3px var(--ui-accent-soft);
}

.status-note {
  display: flex !important;
  align-items: flex-start;
  gap: 0.2rem;
  padding: 0.55rem 0.65rem;
}

.status-note.is-info {
  border-color: color-mix(in srgb, var(--ui-accent) 30%, transparent);
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
}

@media (max-width: 575.98px) {
  .settings-disclosure {
    justify-content: center;
    width: 100%;
  }
}
</style>
