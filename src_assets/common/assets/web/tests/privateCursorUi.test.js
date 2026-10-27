import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { parse } from '@vue/compiler-sfc'
import { baseParse, parserOptions } from '@vue/compiler-dom'

const source = readFileSync(new URL('../configs/tabs/Inputs.vue', import.meta.url), 'utf8')
const { descriptor, errors } = parse(source)
assert.deepEqual(errors, [])
const ast = baseParse(descriptor.template.content, parserOptions)
const elements = (node) => (node.children ?? []).filter((child) => child.type === 1)
const walk = (node, ancestors = []) => elements(node).flatMap((child) =>
  [{ node: child, ancestors }, ...walk(child, [...ancestors, child])])
const entries = walk(ast)
const attr = (node, name) => node.props.find((prop) => prop.type === 6 && prop.name === name)?.value?.content
const directive = (node, name, arg) => node.props.find((prop) =>
  prop.type === 7 && prop.name === name && prop.arg?.content === arg)?.exp?.content
const guards = (ancestors) => ancestors.map((node) => directive(node, 'if')).filter(Boolean)
const text = (node) => node.children.map((child) => child.content).join('').trim()

// Static source contracts only: no browser, hardware or paired-client coverage.
test('capture toggle is unguarded while AMF stays non-Windows, preserving both models', () => {
  for (const id of ['capture_cursor', 'amf_draw_mouse_cursor']) {
    const { node, ancestors } = entries.find(({ node }) => attr(node, 'id') === id)
    assert.equal(node.tag, 'input')
    assert.equal(attr(node, 'type'), 'checkbox')
    assert.equal(directive(node, 'model'), `config.${id}`)
    assert.deepEqual(guards([...ancestors, node]), id === 'capture_cursor' ? [] : ["platform !== 'windows'"])
  }
})

test('Windows notice accurately describes host cursor support without a disable action', () => {
  const { node: notice, ancestors } = entries.find(({ node }) => node.tag === 'div' &&
    elements(node).some((child) => child.tag === 'p' && text(child).startsWith('PyroWave supports')))
  assert.deepEqual(guards([...ancestors, notice]), ["platform === 'windows'"])
  const [message] = elements(notice)
  assert.deepEqual(elements(notice).map((node) => node.tag), ['p'])
  assert.equal(text(message), 'PyroWave supports host-composited cursor capture using an extra GPU copy and requires VDD cursor metadata. Client-side local cursor rendering remains unsupported. Hardware correctness and performance are unverified.')
  assert.equal(elements(message).length, 0)
  assert.ok(message.props.every((prop) => prop.type === 6))
  assert.doesNotMatch(descriptor.template.content, /Disable cursor capture|requires hidden cursor capture/)
  for (const { node } of entries) {
    assert.ok(node.props.every((prop) => prop.type !== 7 || prop.name === 'model' ||
      !/config\.(?:capture_cursor|amf_draw_mouse_cursor)/.test(prop.exp?.content ?? '')))
  }
})

test('no script cursor normalization or watcher is added and mouse stays unguarded', () => {
  assert.doesNotMatch(descriptor.scriptSetup.content, /capture_cursor|amf_draw_mouse_cursor|\bwatch(?:Effect)?\b/)
  const { node, ancestors } = entries.find(({ node }) => attr(node, 'id') === 'mouse')
  assert.equal(directive(node, 'model'), 'config.mouse')
  assert.deepEqual(guards([...ancestors, node]), [])
})
