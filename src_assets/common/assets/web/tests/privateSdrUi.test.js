import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { parse } from '@vue/compiler-sfc'
import { baseParse } from '@vue/compiler-dom'

const sfc = (path) => {
  const source = readFileSync(new URL(path, import.meta.url), 'utf8')
  const { descriptor, errors } = parse(source)
  assert.deepEqual(errors, [])
  return { source, ast: baseParse(descriptor.template.content), script: descriptor.scriptSetup.content }
}
const display = sfc('../configs/tabs/audiovideo/DisplayDeviceOptions.vue')
const advanced = sfc('../configs/tabs/Advanced.vue')
const audioVideo = sfc('../configs/tabs/AudioVideo.vue')
const elements = (node) => (node.children ?? []).filter((child) => child.type === 1)
const descendants = (node) => elements(node).flatMap((child) => [child, ...descendants(child)])
const attr = (node, name) => node.props.find((prop) => prop.type === 6 && prop.name === name)?.value?.content
const directive = (node, name, arg) => node.props.find((prop) =>
  prop.type === 7 && prop.name === name && prop.arg?.content === arg)?.exp?.content
const models = (node) => descendants(node).map((child) => directive(child, 'model')).filter(Boolean)

// Static production-source contracts, not browser/mount or hardware coverage.
test('display options remain Windows-only with empty Linux and macOS slots', () => {
  const layout = elements(display.ast)[0]
  assert.equal(layout.tag, 'PlatformLayout')
  assert.equal(directive(layout, 'bind', 'platform'), 'platform')
  const slots = elements(layout)
  assert.deepEqual(slots.map((node) => node.props.find((prop) => prop.name === 'slot').arg.content), ['windows', 'linux', 'macos'])
  assert.ok(elements(slots[0]).length)
  for (const slot of slots.slice(1)) assert.equal(slot.children.length, 0)
  for (const tag of ['NewDisplayOutputSelector', 'DisplayDeviceOptions']) {
    const node = descendants(audioVideo.ast).find((child) => child.tag === tag)
    assert.equal(directive(node, 'bind', 'platform'), 'platform')
    assert.equal(directive(node, 'bind', 'config'), 'config')
  }
})

test('display preparation, resolution and refresh bindings retain their choices', () => {
  assert.deepEqual(models(display.ast), ['config.display_device_prep', 'config.resolution_change', 'config.manual_resolution', 'config.refresh_rate_change', 'config.manual_refresh_rate'])
  assert.equal(descendants(display.ast).find((node) => node.tag === 'DisplayPreparationPicker')?.tag, 'DisplayPreparationPicker')
  for (const node of descendants(display.ast).filter((child) => child.tag === 'DisplayRuleRadioGroup')) {
    assert.equal(directive(node, 'bind', 'options'), "['no_operation', 'automatic', 'manual']")
    assert.equal(directive(node, 'model'), `config.${attr(node, 'name')}`)
  }
})

test('unsupported HDR and dynamic resizing have no controls, polling or automatic config writes', () => {
  assert.doesNotMatch(display.source, /hdr_|hdrAnalysis|hdrRuntime|hdr-feature|feature-status|dynamic_resolution|Checkbox/)
  assert.doesNotMatch(display.script, /computed|onMounted|onUnmounted|watch|fetch|setTimeout|setInterval|visibilitychange/)
  assert.doesNotMatch(display.script, /config(?:\.value)?(?:\.[\w]+|\[[^\]]+\])\s*=(?!=)|Object\.assign|delete\s|\bfunction\b|=>/)
  assert.match(display.script, /const config = ref\(props.config\)/)
  const note = descendants(display.ast).find((node) => node.tag === 'p' && node.loc.source.includes('PyroWave requires SDR'))
  assert.equal(note.children.map((node) => node.content).join('').trim(), 'PyroWave requires SDR and a fixed display size matching the negotiated stream. HDR and dynamic resizing are unsupported.')
  assert.ok(note.props.every((prop) => prop.type === 6))
  assert.equal(elements(note).length, 0)
})

test('Advanced drops only the HDR manager integration and retains remaining selectors', () => {
  assert.doesNotMatch(advanced.source, /nativeRtxHdr|openManager|hdr_enhanced|onMounted|onBeforeUnmount|\bwatch(?:Effect)?\b/)
  for (const [id, model] of Object.entries({ codec_strategy: 'codecStrategy', codec_enable_hdr: 'enableHdr', hevc_mode: 'config.hevc_mode', av1_mode: 'config.av1_mode', capture: 'config.capture', encoder: 'config.encoder' })) {
    const node = descendants(advanced.ast).find((child) => attr(child, 'id') === id)
    assert.equal(directive(node, 'model'), model)
  }
  for (const tag of ['AdapterNameSelector', 'CaptureCompatibilityOverrides', 'VddPrerequisiteNotice']) {
    assert.ok(descendants(advanced.ast).some((node) => node.tag === tag))
  }
  assert.match(advanced.source, /PyroWave \(SDR BT\.709 YUV420\)/)
})
