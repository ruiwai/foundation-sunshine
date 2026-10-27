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

test('Windows capture exposes exactly DDX and VDD plus the shared Auto option', () => {
  assert.deepEqual(values(slot('windows')), ['ddx', 'vdd'])
  assert.deepEqual(values(capture), [''])
  assert.match(elements(capture)[0].loc.source, /\$t\('_common.autodetect'\)/)
  assert.equal(elements(slot('windows'))[0].children[0].content, 'Desktop Duplication (DDX)')
  assert.match(elements(slot('windows'))[1].loc.source, /\$t\('config.capture_vdd_direct'\)/)
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
test('only explicit Windows VDD activates prerequisites; Auto explains DDX independence', () => {
  assert.match(script, /const isVDDCaptureSelected = computed\(\(\) => \{\s*return props.platform === 'windows' && config.value.capture === 'vdd'\s*\}\)/)
  const notices = descendants(ast).filter((node) => node.tag === 'VddPrerequisiteNotice')
  assert.equal(notices.length, 1)
  const [notice] = notices
  assert.equal(directive(notice, 'bind', 'active')?.exp.content, 'isVDDCaptureSelected')
  const explanation = descendants(ast).find((node) => node.tag === 'span' &&
    directive(node, 'if')?.exp.content === 'isVDDCaptureSelected')
  assert.equal(explanation.children.filter((node) => node.type === 2).map((node) => node.content).join('').trim(),
    'Explicit VDD direct capture requires driver frame export; no capture fallback.')
  const autoNotice = descendants(ast).find((node) => node.tag === 'span' &&
    directive(node, 'if')?.exp.content === "platform === 'windows'")
  assert.equal(autoNotice.children[0].content,
    'Auto uses Desktop Duplication (DDX) on the selected display; no driver frame export is required.')
  assert.doesNotMatch(source, /config.capture_vdd_direct_desc/)
})

test('saved capture is not normalized and mounting does not write configuration', () => {
  assert.doesNotMatch(script, /config(?:\.value)?\.capture\s*=(?!=)/)
  assert.doesNotMatch(script, /\b(?:onMounted|watch|watchEffect)\b/)
})
