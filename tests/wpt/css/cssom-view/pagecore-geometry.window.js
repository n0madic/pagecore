test(() => {
  document.body.style.margin = '0';
  const box = document.createElement('div');
  box.style.cssText = 'width:120px;height:40px;padding:5px;border:2px solid black;margin:0';
  document.body.append(box);

  const rect = box.getBoundingClientRect();
  assert_equals(Math.round(rect.width), 134);
  assert_equals(Math.round(rect.height), 54);
  assert_equals(box.offsetWidth, 134);
  assert_equals(box.offsetHeight, 54);
}, 'CSSOM View geometry is backed by layout');
