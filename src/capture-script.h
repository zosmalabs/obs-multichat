#pragma once

static constexpr const char *capture_script = R"SCRIPT((() => {
  if (window.__zosmaBadgeCapture) return;
  window.__zosmaBadgeCapture = true;
  const platform = location.hostname.endsWith('kick.com') ? 'Kick' : 'Twitch';
  const endpoint = '__ZOSMA_ENDPOINT__';
  const sent = new Map();
  const selector = platform === 'Twitch'
    ? '.chat-line__message, [data-a-target="chat-line-message"]'
    : '.chat-entry, [data-chat-entry], .chat-message';
  const nameSelector = platform === 'Twitch'
    ? '.chat-author__display-name, [data-a-target="chat-message-username"]'
    : 'button.inline.font-bold[data-prevent-expand], button.font-bold.inline, .chat-entry-username, .chat-message-identity button[title]';
  const badgeSelector = platform === 'Twitch'
    ? 'img.chat-badge, .seventv-chat-badge img'
    : '.badge-tooltip img, .badge-tooltip svg, .base-badge img, .base-badge svg, .badge img, .badge svg';
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
    for (const row of rows) {
      const name = row.querySelector(nameSelector)?.textContent?.trim();
      if (!name) continue;
      const badgeRoot = platform === 'Kick' ? row.querySelector('.chat-message-identity') || row : row;
      const badgeNodes = [...badgeRoot.querySelectorAll(badgeSelector)].filter(n => !n.closest('.chat-entry-content, .chat-line__message--emote'));
      if (!badgeNodes.length) continue;
      const signature = badgeNodes.map(n => n.outerHTML).join('|');
      if (sent.get(name) === signature) continue;
      sent.set(name, signature);
      if (sent.size > 100) sent.delete(sent.keys().next().value);
      const badges = [];
      for (const node of badgeNodes.slice(0, 15)) {
        const image = await imageFor(node);
        if (image) badges.push({label: node.getAttribute('alt') || node.getAttribute('aria-label') || node.getAttribute('title') || 'Badge', image});
      }
      if (badges.length) fetch(endpoint, {method:'POST', headers:{'Content-Type':'text/plain'}, body:JSON.stringify({platform,name,badges})}).catch(() => {});
    }
  }
  const observer = new MutationObserver(() => { clearTimeout(window.__zosmaScanTimer); window.__zosmaScanTimer = setTimeout(scan, 100); });
  function start() { observer.observe(document.documentElement, {subtree:true,childList:true,attributes:true,attributeFilter:['src']}); scan(); }
  if (document.documentElement) start(); else document.addEventListener('DOMContentLoaded', start, {once:true});
})())SCRIPT";
