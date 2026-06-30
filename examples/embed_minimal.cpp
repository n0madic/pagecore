#include <pagecore/page.hpp>
#include <pagecore/resource_loader.hpp>

#include <iostream>
#include <memory>

int main()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/app.js",
        "document.getElementById('status').textContent = 'JavaScript executed';",
        "text/javascript");

    pagecore::LoadOptions options;
    options.base_url = "https://example.test/index.html";

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<!doctype html>
<html>
  <body>
    <main id="status">Loading</main>
    <script src="/app.js"></script>
  </body>
</html>
)HTML", options.base_url);
    page.run_until_idle();

    const auto status = page.text_content("#status");
    std::cout << (status ? *status : "<missing>") << "\n";
    return status && *status == "JavaScript executed" ? 0 : 1;
}
