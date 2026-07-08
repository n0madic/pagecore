promise_test(async () => {
  const response = await fetch('/fetch/api/resources/data.json', {
    headers: { 'X-PageCore-Smoke': '1' }
  });
  assert_equals(response.status, 200);
  assert_equals(response.headers.get('content-type'), 'application/json');

  const data = JSON.parse(await response.text());
  assert_equals(data.value, 'ok');
}, 'fetch resolves resources through the WPT loader');
