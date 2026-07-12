// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-actions.js
// META: script=/resources/testdriver-vendor.js

promise_test(async () => {
  const button = document.createElement('button');
  button.textContent = 'click me';
  document.body.append(button);

  const events = [];
  for (const type of ['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click']) {
    button.addEventListener(type, (event) => {
      events.push(`${type}:${event.clientX},${event.clientY},${event.button}`);
    });
  }

  await test_driver.click(button);

  assert_equals(events.length, 5, 'pointerdown, mousedown, pointerup, mouseup and click should all fire');
  assert_true(events[0].startsWith('pointerdown:'), 'pointerdown should fire first');
  assert_true(events[1].startsWith('mousedown:'), 'mousedown should fire second');
  assert_true(events[2].startsWith('pointerup:'), 'pointerup should fire third');
  assert_true(events[3].startsWith('mouseup:'), 'mouseup should fire fourth');
  assert_true(events[4].startsWith('click:'), 'click should fire last');
}, 'test_driver.click() dispatches a real pointer/mouse/click event sequence');

promise_test(async () => {
  const input = document.createElement('input');
  input.type = 'text';
  document.body.append(input);

  await test_driver.send_keys(input, 'ab');

  assert_equals(input.value, 'ab', 'send_keys should type the given characters into the input');
  assert_equals(document.activeElement, input, 'send_keys should focus the target element');
}, 'test_driver.send_keys() types into a focused text input');

promise_test(async () => {
  const target = document.createElement('div');
  target.style.cssText = 'width:50px;height:50px;background:green';
  document.body.append(target);

  let clicked = false;
  target.addEventListener('click', () => { clicked = true; });

  await new test_driver.Actions()
    .pointerMove(0, 0, {origin: target})
    .pointerDown()
    .pointerUp()
    .send();

  assert_true(clicked, 'a pointerMove+pointerDown+pointerUp Actions sequence over an element should fire a click on it');
}, 'test_driver.Actions() synthesizes a single-pointer-source click');
