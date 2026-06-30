(function() {
  "use strict";

  var status = document.getElementById("js-status");
  status.textContent = "JS DOM/style OK";
  status.classList.add("mutated");
  status.dataset.state = "ready";

  var zone = document.getElementById("dom-zone");
  zone.classList.add("ready");
  zone.style.borderColor = "#236f52";

  var tags = ["createElement", "appendChild", "querySelectorAll", "dataset"];
  for (var i = 0; i < tags.length; i += 1) {
    var item = document.createElement("span");
    item.className = "dom-item";
    item.setAttribute("data-index", String(i + 1));
    item.textContent = tags[i];
    zone.appendChild(item);
  }

  var eventTarget = document.getElementById("event-target");
  eventTarget.addEventListener("pagecore-ready", function(event) {
    eventTarget.textContent = "CustomEvent OK: " + event.detail;
    eventTarget.classList.add("mutated");
  });
  eventTarget.dispatchEvent(new CustomEvent("pagecore-ready", { detail: "ok" }));

  setTimeout(function() {
    var timer = document.getElementById("timer-status");
    timer.textContent = "timer OK";
    timer.classList.add("mutated");
  }, 10);
}());
