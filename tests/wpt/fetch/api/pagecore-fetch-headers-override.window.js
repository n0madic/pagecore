promise_test(async () => {
  const response = await fetch('/fetch/api/resources/mime-override.txt');
  assert_equals(response.status, 200);
  // Regression test for WptResourceLoader: a .headers sidecar's explicit
  // Content-Type must win over the extension-guessed MIME type.
  assert_equals(response.headers.get('content-type'), 'application/x-pagecore-test');
}, 'a .headers sidecar overrides the extension-guessed Content-Type');
