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

static constexpr const char *tiktok_capture_script = R"SCRIPT((() => {
  if (window.top !== window.self || window.__zosmaTikTokCapture || !/(^|\.)tiktok\.com$/.test(location.hostname)) return;
  window.__zosmaTikTokCapture = true;
  const seen = new WeakMap();
  const pending = [];
  let initialized = false;
  let lastStatus = 0;
  let lastAccount = '';
  let sequence = 0;
  const rowsSelector = '[data-e2e="chat-message"]';
  function silenceMedia() {
    for (const media of document.querySelectorAll('video, audio')) {
      if (!media.muted) media.muted = true;
      if (media.volume !== 0) media.volume = 0;
    }
  }
  document.addEventListener('play', silenceMedia, true);
  document.addEventListener('volumechange', silenceMedia, true);
  setInterval(silenceMedia, 500);
  const enqueue = data => { if (pending.length < 100) pending.push(data); };
  setInterval(() => {
    if (!pending.length) return;
    try {
      document.title = 'zosma:' + btoa(unescape(encodeURIComponent(JSON.stringify({...pending.shift(), sequence: ++sequence}))));
    } catch (_) {}
  }, 130);
  function scan() {
    silenceMedia();
    const profileLink = document.querySelector('a[data-e2e="profile-icon"][href*="/@"], a[data-e2e="profile-link"][href*="/@"], [data-e2e="profile-icon"] a[href*="/@"]');
    const profilePath = profileLink ? new URL(profileLink.href, location.href).pathname : location.pathname;
    const account = profilePath.match(/^\/@([A-Za-z0-9._]{2,30})\/?$/)?.[1] || '';
    if (account && account !== lastAccount) {
      lastAccount = account;
      enqueue({type:'account', platform:'TikTok', name:account, url:location.href});
    }
    let rows = [...document.querySelectorAll(rowsSelector)].slice(-80);
    if (!rows.length) rows = [...document.querySelectorAll('[data-index].w-full')].slice(-80);
    for (const row of rows) {
      const nameNode = row.querySelector('[data-e2e="message-owner-name"]');
      const name = (nameNode?.textContent || nameNode?.getAttribute('title') || '').trim().slice(0, 80);
      const messageNode = row.querySelector('[class*="-DivComment"], .live-shared-ui-chat-list-chat-message-comment, [data-e2e="chat-message"] .break-words.align-middle') ||
        nameNode?.closest('[class*="DivUserInfo"]')?.nextElementSibling;
      let message = (messageNode?.textContent || '').trim().slice(0, 500);
      if (!message && nameNode) {
        const content = (row.textContent || '').trim();
        if (content.startsWith(name)) message = content.slice(name.length).trim().slice(0, 500);
      }
      if (!name || !message) continue;
      const signature = name + '\n' + message;
      if (seen.get(row) === signature) continue;
      seen.set(row, signature);
      if (initialized) enqueue({type:'chat', platform:'TikTok', name, message});
    }
    if (rows.length) initialized = true;
    if (Date.now() - lastStatus > 5000) {
      lastStatus = Date.now();
      const bodyText = (document.body?.innerText || '').slice(0, 1200).toLowerCase();
      enqueue({type:'status', platform:'TikTok', rows:rows.length, url:location.href,
        chatSurface:!!document.querySelector('[data-e2e="chat-room"], [data-e2e="live-chat-container"], [data-e2e="public-screen-live-chat-slot"], [class*="DivChatRoomContent"], .live-shared-ui-chat-list-scrolling-list'),
        video:!!document.querySelector('video'),
        login:!!document.querySelector('a[href*="/login"], button[data-e2e*="login"]') || /log in|sign in|entrar na conta/.test(bodyText),
        verification:!!document.querySelector('[id*="captcha"], [class*="captcha"], iframe[src*="verify"]'),
        visibility:document.visibilityState});
    }
  }
  const observer = new MutationObserver(() => {
    clearTimeout(window.__zosmaTikTokScanTimer);
    window.__zosmaTikTokScanTimer = setTimeout(scan, 120);
  });
  function start() {
    scan();
    observer.observe(document.documentElement, {subtree:true, childList:true, characterData:true});
  }
  if (document.documentElement) start();
  else document.addEventListener('DOMContentLoaded', start, {once:true});
  setInterval(scan, 3000);
})())SCRIPT";
