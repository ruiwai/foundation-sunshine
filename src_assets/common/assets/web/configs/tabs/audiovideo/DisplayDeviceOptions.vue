<script setup>
import { ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'
import DisplayPreparationPicker from './DisplayPreparationPicker.vue'
import DisplayRuleRadioGroup from './DisplayRuleRadioGroup.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #windows>
      <div class="mb-3 accordion">
        <div class="accordion-item">
          <h2 class="accordion-header" id="panelsStayOpen-headingOne">
            <button
              class="accordion-button"
              type="button"
              data-bs-toggle="collapse"
              data-bs-target="#panelsStayOpen-collapseOne"
            >
              {{ $tp('config.display_device_options') }}
            </button>
          </h2>
          <div
            id="panelsStayOpen-collapseOne"
            class="accordion-collapse collapse show"
            aria-labelledby="panelsStayOpen-headingOne"
          >
            <div class="accordion-body">
              <p class="form-text">
                PyroWave requires SDR and a fixed display size matching the negotiated stream. HDR and dynamic resizing are unsupported.
              </p>
              <DisplayPreparationPicker v-model="config.display_device_prep" />

              <details class="display-options-note">
                <summary>
                  <i class="fas fa-circle-info" aria-hidden="true"></i>
                  {{ $tp('config.display_device_options_note') }}
                </summary>
                <p class="pre-line">{{ $tp('config.display_device_options_note_desc') }}</p>
              </details>

              <div class="display-rule-grid">
                <DisplayRuleRadioGroup
                  v-model="config.resolution_change"
                  name="resolution_change"
                  label-key="config.resolution_change"
                  option-key-prefix="config.resolution_change_"
                  :options="['no_operation', 'automatic', 'manual']"
                >
                  <div
                    class="form-text"
                    v-if="config.resolution_change === 'automatic' || config.resolution_change === 'manual'"
                  >
                    {{ $tp('config.resolution_change_ogs_desc') }}
                  </div>

                  <div class="nested-setting mt-2" v-if="config.resolution_change === 'manual'">
                    <div class="form-text">
                      {{ $tp('config.resolution_change_manual_desc') }}
                    </div>
                    <input
                      type="text"
                      class="form-control"
                      id="manual_resolution"
                      placeholder="2560x1440"
                      v-model="config.manual_resolution"
                    />
                  </div>
                </DisplayRuleRadioGroup>

                <DisplayRuleRadioGroup
                  v-model="config.refresh_rate_change"
                  name="refresh_rate_change"
                  label-key="config.refresh_rate_change"
                  option-key-prefix="config.refresh_rate_change_"
                  :options="['no_operation', 'automatic', 'manual']"
                >
                  <div class="nested-setting mt-2" v-if="config.refresh_rate_change === 'manual'">
                    <div class="form-text">
                      {{ $tp('config.refresh_rate_change_manual_desc') }}
                    </div>
                    <input
                      type="text"
                      class="form-control"
                      id="manual_refresh_rate"
                      placeholder="59.95"
                      v-model="config.manual_refresh_rate"
                    />
                  </div>
                </DisplayRuleRadioGroup>
              </div>
            </div>
          </div>
        </div>
      </div>
    </template>
    <template #linux> </template>
    <template #macos> </template>
  </PlatformLayout>
</template>

<style scoped>
.display-options-note {
  margin: 0 0 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-accent-soft);
  color: var(--ui-text-secondary);
}

.display-options-note summary {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.7rem 0.85rem;
  color: var(--ui-text-primary);
  font-size: 0.85rem;
  font-weight: 600;
  cursor: pointer;
  list-style: none;
}

.display-options-note summary::-webkit-details-marker {
  display: none;
}

.display-options-note summary i {
  color: var(--ui-accent);
}

.display-options-note summary::after {
  margin-left: auto;
  font-family: 'Font Awesome 7 Free';
  font-weight: 900;
  content: '\f078';
  transition: transform 0.2s ease;
}

.display-options-note[open] summary::after {
  transform: rotate(180deg);
}

.display-options-note p {
  margin: 0;
  padding: 0 0.85rem 0.85rem;
  font-size: 0.82rem;
  line-height: 1.5;
}

.display-rule-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 13rem), 1fr));
  gap: 0.75rem;
  margin-bottom: 1rem;
}

.nested-setting {
  padding: 0.75rem;
  border-left: 3px solid var(--ui-border-strong);
  border-radius: 0 var(--ui-radius-sm) var(--ui-radius-sm) 0;
  background: var(--ui-surface-strong);
}

@media (max-width: 575.98px) {
  .display-options-note,
  .nested-setting {
    padding: 0.75rem;
  }

  .display-options-note {
    padding: 0;
  }

}
</style>
