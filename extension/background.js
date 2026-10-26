// Answers content.js with the URLs of the YouTube tabs open right now.
//
// The extension only has host permissions for YouTube (no "tabs" permission),
// so the browser reveals the URL of YouTube tabs only; every other tab comes
// back without a URL and is skipped. Incognito tabs are excluded unless you
// allow the extension there.

const YOUTUBE = /^https?:\/\/([a-z0-9-]+\.)*(youtube\.com|youtu\.be)\//i;

chrome.runtime.onMessage.addListener((msg, _sender, reply) => {
  if (msg?.type !== "tabs") return false;
  chrome.tabs.query({}).then(
    (tabs) => reply({ urls: tabs.map((t) => t.url || t.pendingUrl).filter((u) => u && YOUTUBE.test(u)) }),
    (err) => reply({ error: String(err) }),
  );
  return true; // the reply is sent asynchronously
});
