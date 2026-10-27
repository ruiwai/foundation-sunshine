import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { parse } from '@vue/compiler-sfc'
import { baseParse } from '@vue/compiler-dom'

const read = (path) => readFileSync(new URL(path, import.meta.url), 'utf8')
const sfc = (path) => {
  const { descriptor, errors } = parse(read(path))
  assert.deepEqual(errors, [])
  return { ast: baseParse(descriptor.template.content), script: descriptor.scriptSetup.content }
}
const advanced = sfc('../configs/tabs/Advanced.vue')
const view = sfc('../views/Config.vue')
const composable = read('../composables/useConfig.js')
const elements = (node) => (node.children ?? []).filter((child) => child.type === 1)
const descendants = (node) => elements(node).flatMap((child) => [child, ...descendants(child)])
const attr = (node, name) => node.props.find((prop) => prop.type === 6 && prop.name === name)?.value?.content
const directive = (node, name) => node.props.find((prop) => prop.type === 7 && prop.name === name)?.exp?.content
const panels = elements(elements(advanced.ast)[0])

// Static production-source contracts only; not browser acceptance or mount coverage.
test('legacy codec strategy and encoder selection are gated off on Windows', () => {
  for (const id of ['codec_strategy', 'encoder']) {
    const panel = panels.find((node) => descendants(node).some((child) => attr(child, 'id') === id))
    assert.equal(directive(panel, 'if'), "platform !== 'windows'")
  }
  const codecPanel = panels.find((node) => node.loc.source.includes('id="codec_strategy"'))
  for (const id of ['codec_enable_hdr', 'hevc_mode', 'av1_mode']) {
    assert.ok(descendants(codecPanel).some((node) => attr(node, 'id') === id))
  }
})

test('Windows encoder panel is translated, literal PyroWave information with no controls', () => {
  const panel = panels.find((node) => node.loc.source.includes('PyroWave (SDR BT.709 YUV420)'))
  assert.equal(directive(panel, 'if'), "platform === 'windows'")
  assert.match(panel.loc.source, /\$t\('config.encoder'\)/)
  for (const node of [panel, ...descendants(panel)]) {
    assert.ok(!['input', 'select', 'button', 'textarea'].includes(node.tag))
    assert.equal(directive(node, 'model'), undefined)
    assert.ok(!node.props.some((prop) => prop.type === 7 && prop.name === 'on'))
  }
})

test('navigation filters only the Windows encoder group and preserves non-Windows tabs', () => {
  assert.match(view.script, /tabs: configTabs,[\s\S]*?\} = useConfig\(\)/)
  assert.match(view.script, /const tabs = computed\(\(\) => platform.value === 'windows'\s*\? configTabs.value.filter\(\(tab\) => tab.id !== 'encoders'\)\s*: configTabs.value\)/)
  assert.doesNotMatch(view.script, /configTabs.value\s*=(?!=)|delete config\b/)
  const container = descendants(view.ast).find((node) => node.tag === 'ContainerEncoders')
  assert.equal(directive(container, 'if'), 'isEncoderCurrentTab')
  assert.match(view.script, /const isEncoderCurrentTab = computed\(\(\) => platform.value !== 'windows' && ENCODER_TAB_IDS.has\(currentTab.value\)\)/)
})

test('resolved legacy hashes redirect Windows before preferred encoder selection', () => {
  assert.match(view.script, /const ENCODER_TAB_IDS = new Set\(\['nv', 'qsv', 'amd', 'vt', 'sw'\]\)/)
  const handler = view.script.match(/const handleConfigHash = \(\) => \{([\s\S]*?)^\}/m)?.[1]
  assert.ok(handler)
  assert.match(handler, /^\s*handleHash\(\)\s*if \(platform.value === 'windows' && \(currentTab.value === 'encoders' \|\| ENCODER_TAB_IDS.has\(currentTab.value\)\)\) \{\s*currentTab.value = 'advanced'\s*return\s*\}\s*if \(currentTab.value === 'encoders'\)/)
  assert.match(handler, /currentTab.value = preferredEncoderTab\(encoderGroup\?\.children\)/)
  assert.match(composable, /child.id === hash \|\| Object.keys\(child.options\).includes\(hash\)/)
  assert.match(composable, /const targetTab = findTabByHash\(hash\)[\s\S]*?currentTab.value = targetTab.id/)
})

test('composable retains saved-setting defaults and tab-driven serialization independently of view tabs', () => {
  for (const field of ['hevc_mode', 'av1_mode', 'encoder', 'nvenc_preset', 'nvenc_vdd_force_vsync']) {
    assert.match(composable, new RegExp(`\\b${field}:`))
  }
  assert.match(composable, /const fillDefaultValues = \(\) => \{\s*forEachTabOption\(tabs.value,/)
  assert.match(composable, /const removeDefaultValues = \(configData\) => \{\s*forEachTabOption\(tabs.value,/)
  assert.doesNotMatch(composable, /configTabs|PyroWave/)
})
