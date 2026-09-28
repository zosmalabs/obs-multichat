#pragma once

static constexpr const char *capture_script = R"SCRIPT((() => {
  if (window.__zosmaBadgeCapture) return;
  if (!location.hostname.endsWith('kick.com') && !location.hostname.endsWith('twitch.tv')) return;
  window.__zosmaBadgeCapture = true;
  const platform = location.hostname.endsWith('kick.com') ? 'Kick' : 'Twitch';
  let sequence = 0;
  function report(payload) {
    try {
      document.title = 'zosma:' + btoa(unescape(encodeURIComponent(JSON.stringify({...payload, sequence: ++sequence}))));
    } catch (_) {}
  }
  const sent = new Map();
  const selector = platform === 'Twitch'
    ? '.chat-line__message, [data-a-target="chat-line-message"]'
    : '[data-index], [data-chat-entry], .chat-entry, .chat-message';
  const nameSelector = platform === 'Twitch'
    ? '.chat-author__display-name, [data-a-target="chat-message-username"]'
    : 'button.inline.font-bold[data-prevent-expand], button.font-bold.inline, .chat-entry-username, .chat-message-identity button[title]';
  const badgeSelector = platform === 'Twitch'
    ? 'img.chat-badge, .seventv-chat-badge img'
    : 'svg[data-ds-icon], img[src], .badge-tooltip svg';
  async function imageFor(node) {
    if (node.tagName !== 'svg') return node.currentSrc || node.src || '';
    try {
      const svg = new XMLSerializer().serializeToString(node);
      const image = new Image();
      const src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
      await new Promise((resolve, reject) => { image.onload = resolve; image.onerror = reject; image.src = src; });
      const canvas = document.createElement('canvas');
      canvas.width = 40; canvas.height = 40;
      canvas.getContext('2d').drawImage(image, 0, 0, 40, 40);
      return canvas.toDataURL('image/png');
    } catch (_) { return ''; }
  }
  async function scan() {
    const rows = [...document.querySelectorAll(selector)].slice(-45);
    let found = 0;
    for (const row of rows) {
      const nameNode = row.querySelector(nameSelector);
      const name = nameNode?.textContent?.trim();
      if (!name) continue;
      const badgeRoot = platform === 'Kick' ? row.querySelector('.chat-message-identity') || nameNode.parentElement || row : row;
      const badgeNodes = [...badgeRoot.querySelectorAll(badgeSelector)].filter(n =>
        platform === 'Twitch' ||
        (!n.closest('button, .chat-entry-content, .chat-line__message--emote') &&
         (n.tagName.toLowerCase() === 'svg' ||
          /\/chat\/badges\/|\/channel_subscriber_badges\//.test(n.currentSrc || n.src || ''))));
      if (!badgeNodes.length) continue;
      found += badgeNodes.length;
      const signature = badgeNodes.map(n => n.outerHTML).join('|');
      if (sent.get(name) === signature) continue;
      sent.set(name, signature);
      if (sent.size > 100) sent.delete(sent.keys().next().value);
      const badges = [];
      for (const node of badgeNodes.slice(0, 15)) {
        const image = await imageFor(node);
        if (image) badges.push({label: node.getAttribute('alt') || node.getAttribute('aria-label') || node.getAttribute('title') || 'Badge', image});
      }
      if (badges.length) report({platform,name,badges});
    }
    if (!window.__zosmaStatusAt || Date.now() - window.__zosmaStatusAt > 5000) {
      window.__zosmaStatusAt = Date.now();
      report({type:'status',platform,rows:rows.length,found,url:location.href});
    }
  }
  const observer = new MutationObserver(() => { clearTimeout(window.__zosmaScanTimer); window.__zosmaScanTimer = setTimeout(scan, 100); });
  function start() { observer.observe(document.documentElement, {subtree:true,childList:true,attributes:true,attributeFilter:['src']}); scan(); }
  if (document.documentElement) start(); else document.addEventListener('DOMContentLoaded', start, {once:true});
  setInterval(scan, 8000);
})())SCRIPT";

