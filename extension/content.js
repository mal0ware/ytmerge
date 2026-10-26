// Runs only on the ytmerge page (http://localhost:8765). The page posts a
// "ytmerge:tabs-request"; we acknowledge it at once (so the page knows the
// extension is installed), ask background.js for the open YouTube tabs, and
// post them back as "ytmerge:tabs".

addEventListener("message", (e) => {
  if (e.source !== window || e.origin !== location.origin) return;
  const { type, id } = e.data || {};
  if (type !== "ytmerge:tabs-request") return;
  const answer = (data) => postMessage({ type: "ytmerge:tabs", id, ...data }, location.origin);
  postMessage({ type: "ytmerge:tabs-ack", id }, location.origin);
  chrome.runtime.sendMessage({ type: "tabs" }).then(
    (res) => answer(res || { error: "no answer from the extension" }),
    (err) => answer({ error: String(err) }),
  );
});
