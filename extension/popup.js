const HOST = "com.fdm.native_host";
const enabledBox = document.getElementById("enabled");
const statusDot = document.getElementById("statusDot");
const statusText = document.getElementById("statusText");
const recheckBtn = document.getElementById("recheck");

// --- toggle ---------------------------------------------------------------
chrome.storage.local.get({ enabled: true }).then(({ enabled }) => {
  enabledBox.checked = enabled;
});

enabledBox.addEventListener("change", () => {
  chrome.storage.local.set({ enabled: enabledBox.checked });
});

// --- connection status ----------------------------------------------------
// state: "checking" | "ok" | "bad"
function renderStatus(state, text) {
  statusDot.className = "dot " + state;
  statusText.textContent = text;
}

function checkConnection() {
  renderStatus("checking", "Checking…");
  // ping = liveness check; the host answers without opening a download dialog.
  chrome.runtime.sendNativeMessage(HOST, { ping: true }, (resp) => {
    if (chrome.runtime.lastError) {
      const msg = chrome.runtime.lastError.message || "Native host not registered";
      console.error("FDM native messaging error:", msg);
      renderStatus("bad", msg);
      statusText.title = msg;  // full text on hover (the chip truncates)
    } else if (resp && resp.ok) {
      renderStatus("ok", "Connected to FDM");
    } else {
      renderStatus("bad", "Error: " + (resp && resp.error));
    }
  });
}

recheckBtn.addEventListener("click", checkConnection);
checkConnection();
