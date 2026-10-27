import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { parse } from '@vue/compiler-sfc'
import { baseParse } from '@vue/compiler-dom'

const source = readFileSync(new URL('../configs/tabs/Advanced.vue', import.meta.url), 'utf8')
const { descriptor, errors } = parse(source)
assert.deepEqual(errors, [])
const ast = baseParse(descriptor.template.content)
const script = descriptor.scriptSetup.content
const elements = (node) => (node.children ?? []).filter((child) => child.type === 1)
const descendants = (node) => elements(node).flatMap((child) => [child, ...descendants(child)])
const attr = (node, name) => node.props.find((prop) => prop.type === 6 && prop.name === name)?.value?.content
const directive = (node, name, arg) => node.props.find((prop) =>
  prop.type === 7 && prop.name === name && prop.arg?.content === arg)
const capture = descendants(ast).find((node) => node.tag === 'select' && attr(node, 'id') === 'capture')
const layout = elements(capture).find((node) => node.tag === 'PlatformLayout')
const slot = (name) => elements(layout).find((node) => directive(node, 'slot', name))
const values = (node) => elements(node).filter((child) => child.tag === 'option').map((child) => attr(child, 'value'))

test('Windows capture exposes only VDD plus the shared Auto option', () => {
  assert.deepEqual(values(slot('windows')), ['vdd'])
  assert.deepEqual(values(capture), [''])
  assert.match(elements(capture)[0].loc.source, /\$t\('_common.autodetect'\)/)
  assert.equal(directive(capture, 'model')?.exp.content, 'config.capture')
  assert.equal(directive(layout, 'bind', 'platform')?.exp.content, 'platform')
})

test('Linux capture choices are unchanged and macOS capture stays hidden', () => {
  assert.deepEqual(values(slot('linux')), ['nvfbc', 'wlr', 'kms', 'x11'])
  const panel = descendants(ast).find((node) => elements(node).includes(capture))
  assert.equal(directive(panel, 'if')?.exp.content, "platform !== 'macos'")
})

test('obsolete WGC service controls and AMD warnings are absent', () => {
  assert.doesNotMatch(source, /isWGCSelected|isAMDCaptureSelected|isTauri|checkSunshineMode|toggleSunshineMode|showMessage/)
  assert.doesNotMatch(source, /isUserMode|isCheckingMode|mode-switch-button|amd_capture_no_virtual_display/)
  assert.doesNotMatch(source, /toggle_sunshine_mode|is_sunshine_running_in_user_mode|\bwatch\b|useI18n/)
})

// Static production-source contracts, not browser/mount integration coverage.
test('Windows Auto and VDD share the prerequisite and explanatory state', () => {
  assert.match(script, /const isVDDCaptureSelected = computed\(\(\) => \{\s*return props.platform === 'windows' && \(config.value.capture === '' \|\| config.value.capture === 'vdd'\)\s*\}\)/)
  const notice = descendants(ast).find((node) => node.tag === 'VddPrerequisiteNotice')
  assert.equal(directive(notice, 'bind', 'active')?.exp.content, 'isVDDCaptureSelected')
  const explanation = descendants(ast).find((node) => node.tag === 'span' &&
    directive(node, 'if')?.exp.content === 'isVDDCaptureSelected')
  assert.match(explanation.loc.source, /config.capture_vdd_direct_desc/)
})

test('saved capture is not normalized and mounting does not write configuration', () => {
  assert.doesNotMatch(script, /config(?:\.value)?\.capture\s*=(?!=)/)
  const mounted = script.match(/onMounted\(\(\) => \{([\s\S]*?)^\}\)/m)
  assert.ok(mounted)
  assert.doesNotMatch(mounted[1], /\bconfig\b/)
})
