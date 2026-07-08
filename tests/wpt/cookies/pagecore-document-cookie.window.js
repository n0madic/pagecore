test(() => {
  document.cookie = 'pagecore_a=1; Path=/';
  document.cookie = 'pagecore_b=2; Path=/';

  const cookie = document.cookie;
  assert_true(cookie.includes('pagecore_a=1'));
  assert_true(cookie.includes('pagecore_b=2'));
}, 'document.cookie stores and exposes same-page cookies');
