const HOST = "com.fdm.native_host";
const enabledBox = document.getElementById("enabled");
const statusEl = document.getElementById("status");

chrome.storage.local.get({ enabled: true }).then(({ enabled }) => {
  enabledBox.checked = enabled;
});

enabledBox.addEventListener("change", () => {
  chrome.storage.local.set({ enabled: enabledBox.checked });
});

document.getElementById("test").addEventListener("click", () => {
  statusEl.textContent = "…";
  // ping = liveness check; the host answers without opening a download dialog.
  chrome.runtime.sendNativeMessage(HOST, { ping: true }, (resp) => {
    if (chrome.runtime.lastError) {
      statusEl.textContent = "host not found";
    } else if (resp && resp.ok) {
      statusEl.textContent = "connected";
    } else {
      statusEl.textContent = "error: " + (resp && resp.error);
    }
  });
});
