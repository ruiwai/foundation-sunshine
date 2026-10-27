import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { parse } from '@vue/compiler-sfc'
import { baseParse } from '@vue/compiler-dom'

const source = readFileSync(new URL('../configs/tabs/advanced/CaptureCompatibilityOverrides.vue', import.meta.url), 'utf8')
const { descriptor, errors } = parse(source)
assert.deepEqual(errors, [])
const ast = baseParse(descriptor.template.content)
const elements = (node) => (node.children ?? []).filter((child) => child.type === 1)
const descendants = (node) => elements(node).flatMap((child) => [child, ...descendants(child)])
const attr = (node, name) => node.props.find((prop) => prop.type === 6 && prop.name === name)?.value?.content
const directive = (node, name, arg) => node.props.find((prop) => prop.type === 7 && prop.name === name && prop.arg?.content === arg)
const layout = elements(ast)[0]
const slot = (name) => elements(layout).find((node) => directive(node, 'slot', name))
const select = descendants(slot('windows')).find((node) => node.tag === 'select')

// Static source contracts only, not browser interaction or mount coverage.
test('capture target is Windows-only under the existing platform binding', () => {
  assert.deepEqual(elements(ast).map((node) => node.tag), ['PlatformLayout'])
  assert.equal(directive(layout, 'bind', 'platform')?.exp.content, 'platform')
  assert.deepEqual(elements(layout).map((node) => node.props[0].arg.content), ['windows', 'linux', 'macos'])
  for (const name of ['linux', 'macos']) assert.equal(slot(name).children.length, 0)
})

test('only explicit display selection is offered with existing translations', () => {
  assert.equal(attr(select, 'id'), 'capture_target')
  assert.equal(directive(select, 'model')?.exp.content, 'config.capture_target')
  assert.deepEqual(descendants(ast).filter((node) => node.tag === 'option').map((node) => attr(node, 'value')), ['display'])
  assert.match(elements(select)[0].loc.source, /\$t\('config.capture_target_display'\)/)
  const label = descendants(slot('windows')).find((node) => node.tag === 'label')
  assert.equal(attr(label, 'for'), 'capture_target')
  assert.match(label.loc.source, /\$t\('config.capture_target'\)/)
})

test('explanation is read-only and obsolete controls and imports are absent', () => {
  assert.ok(source.includes('PyroWave uses display-only VDD capture and direct SDR conversion. Window capture and conversion overrides are unsupported.'))
  assert.doesNotMatch(source, /Checkbox|accordion|window_title|wgc|capture_compute_shader/i)
  assert.deepEqual(descendants(ast).filter((node) => ['input', 'select', 'button', 'textarea'].includes(node.tag)), [select])
  assert.deepEqual(descendants(ast).flatMap((node) => node.props.filter((prop) => prop.type === 7).map((prop) => prop.name)), ['bind', 'slot', 'model', 'slot', 'slot'])
})

test('script only retains props/ref plumbing, with no automatic saved-config writes', () => {
  assert.equal(descriptor.script, null)
  assert.equal(descriptor.scriptSetup.content.replace(/\s+/g, ' ').trim(), "import { ref } from 'vue' import PlatformLayout from '../../../components/layout/PlatformLayout.vue' const props = defineProps({ platform: String, config: Object, }) const config = ref(props.config)")
})
