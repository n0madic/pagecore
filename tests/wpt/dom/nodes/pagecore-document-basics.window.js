test(() => {
  const host = document.createElement('section');
  host.id = 'host';
  host.className = 'alpha beta';
  host.append('hello');
  document.body.append(host);

  assert_equals(document.querySelector('#host').textContent, 'hello');
  assert_true(host.classList.contains('alpha'));
  assert_equals(document.getElementsByClassName('beta').length, 1);
}, 'document creation, selectors, classList and textContent');

test(() => {
  const node = document.createElement('div');
  node.append(document.createTextNode('a'));
  node.append(document.createTextNode(''));
  node.append(document.createTextNode('b'));
  node.normalize();

  assert_equals(node.childNodes.length, 1);
  assert_equals(node.textContent, 'ab');
}, 'Node.normalize merges adjacent text nodes and drops empty nodes');
