<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #windows>
      <div class="my-3">
        <label for="capture_target" class="form-label">{{ $t('config.capture_target') }}</label>
        <select id="capture_target" class="form-select" v-model="config.capture_target">
          <option value="display">{{ $t('config.capture_target_display') }}</option>
        </select>
        <div class="form-text">
          PyroWave uses display-only VDD capture and direct SDR conversion. Window capture and conversion overrides are unsupported.
        </div>
      </div>
    </template>
    <template #linux> </template>
    <template #macos> </template>
  </PlatformLayout>
</template>
