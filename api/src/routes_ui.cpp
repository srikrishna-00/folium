#include "routes_ui.h"

namespace uapp::api {

namespace {

// Raw string literal, not a templating engine: there is nothing dynamic
// here (every API call happens client-side, in the browser, against the
// real routes), so a build-step templating engine would be solving a
// problem this page does not have. No AI/LLM anywhere in this page or the
// routes it calls, by explicit product decision — this is a document/
// notes/PDF app, not a chat app.
//
// LAYOUT, per explicit user feedback that the first version "felt like a
// developer console": a proper Home dashboard (recent pages + a tool-card
// grid, the shape iLovePDF and a WPS-style launcher both use) instead of a
// blank "select a page" screen, and a left nav styled as icon+label rows
// (Home / Notes / PDF Viewer / Tools) with the page TREE underneath as its
// own section — the same split Notion's own sidebar makes between
// top-level destinations and the page list. Every engineering caveat that
// used to sit in a visible hint banner (no server-side "list all pages",
// etc.) moved into code comments here instead — true and worth recording,
// not something a person using the app needs to read.
constexpr const char* kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Notetrack</title>
<style>
  :root {
    --fg:#2b2724; --fg-muted:#8a7f74; --bg:#ffffff; --bg-sidebar:#f7f5f2;
    --accent:#2d6a4f; --accent-soft:#e4ede8; --border:#e8e4de; --hover:#efece6;
    --card-shadow: 0 1px 2px rgba(43,39,36,0.06), 0 1px 1px rgba(43,39,36,0.04);
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    color: var(--fg); background: var(--bg); display: flex; overflow: hidden;
  }

  /* --- sidebar --- */
  #sidebar {
    width: 250px; flex: 0 0 250px; background: var(--bg-sidebar);
    border-right: 1px solid var(--border); display: flex; flex-direction: column;
    height: 100vh; overflow-y: auto;
  }
  #sidebar .brand {
    padding: 1rem 1rem 0.6rem; font-weight: 700; font-size: 1rem;
    display: flex; align-items: center; gap: 0.45rem;
  }
  #sidebar .brand .brand-icon { font-size: 1.15rem; }

  .nav { padding: 0 0.6rem; display: flex; flex-direction: column; gap: 1px; margin-bottom: 0.9rem; }
  .nav-item {
    display: flex; align-items: center; gap: 0.6rem; width: 100%; text-align: left;
    background: none; border: none; padding: 0.42rem 0.6rem; border-radius: 7px;
    font-size: 0.86rem; color: #4a443d; cursor: pointer;
  }
  .nav-item:hover { background: var(--hover); }
  .nav-item.active { background: var(--accent-soft); color: var(--accent); font-weight: 600; }
  .nav-icon { font-size: 1rem; width: 1.1rem; text-align: center; }

  .nav-section-label {
    display: flex; align-items: center; justify-content: space-between;
    padding: 0.3rem 1rem; font-size: 0.68rem; font-weight: 700; letter-spacing: 0.04em;
    color: var(--fg-muted); text-transform: uppercase;
  }
  .nav-section-label .icon-btn {
    background: none; border: none; color: var(--fg-muted); font-size: 0.95rem;
    cursor: pointer; padding: 0.1rem 0.3rem; border-radius: 4px; line-height: 1;
  }
  .nav-section-label .icon-btn:hover { background: var(--hover); color: var(--fg); }

  #page-list { flex: 1; padding: 0 0.5rem; }
  .page-item {
    display: flex; align-items: center; justify-content: space-between; gap: 0.3rem;
    padding: 0.38rem 0.55rem; border-radius: 6px; cursor: pointer; font-size: 0.84rem;
    color: #4a443d;
  }
  .page-item:hover { background: var(--hover); }
  .page-item.selected { background: var(--accent-soft); color: var(--accent); font-weight: 600; }
  .page-item .title { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1;
                       display: flex; align-items: center; gap: 0.4rem; }
  .page-item .del { opacity: 0; color: #b23a3a; font-size: 0.8rem; padding: 0 0.2rem; }
  .page-item:hover .del { opacity: 0.7; }
  .page-item .del:hover { opacity: 1; }
  .empty-pages-note { padding: 0.4rem 1rem; font-size: 0.78rem; color: var(--fg-muted); }

  /* --- main --- */
  #main { flex: 1; height: 100vh; overflow-y: auto; }
  .view { display: none; }
  .view.visible { display: block; }

  /* --- home dashboard --- */
  #view-home { max-width: 920px; margin: 0 auto; padding: 3rem 3rem 5rem; }
  #view-home h1 { font-size: 1.6rem; margin: 0 0 0.3rem; }
  #view-home .subtitle { color: var(--fg-muted); font-size: 0.92rem; margin-bottom: 2.2rem; }
  .section-title { font-size: 0.82rem; font-weight: 700; text-transform: uppercase; letter-spacing: 0.04em;
                    color: var(--fg-muted); margin: 0 0 0.8rem; }
  .card-row { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 0.9rem; margin-bottom: 2.4rem; }
  .recent-card {
    border: 1px solid var(--border); border-radius: 10px; padding: 1rem 1.1rem; cursor: pointer;
    background: #fff; box-shadow: var(--card-shadow); transition: transform 0.08s, box-shadow 0.08s;
  }
  .recent-card:hover { border-color: var(--accent); transform: translateY(-1px); box-shadow: 0 4px 10px rgba(43,39,36,0.08); }
  .recent-card .icon { font-size: 1.3rem; margin-bottom: 0.5rem; }
  .recent-card .title { font-weight: 600; font-size: 0.9rem; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .new-page-card {
    border: 1px dashed #c9c2b8; border-radius: 10px; padding: 1rem 1.1rem; cursor: pointer;
    display: flex; align-items: center; gap: 0.5rem; color: var(--fg-muted); font-size: 0.9rem; font-weight: 600;
  }
  .new-page-card:hover { background: var(--hover); border-style: solid; }

  .tool-card {
    border: 1px solid var(--border); border-radius: 12px; padding: 1.2rem 1.1rem; background: #fff;
    box-shadow: var(--card-shadow); cursor: pointer; transition: transform 0.08s, box-shadow 0.08s;
  }
  .tool-card:hover { transform: translateY(-1px); box-shadow: 0 4px 12px rgba(43,39,36,0.09); border-color: var(--accent); }
  .tool-card .icon { font-size: 1.6rem; margin-bottom: 0.6rem; }
  .tool-card .title { font-weight: 700; font-size: 0.95rem; margin-bottom: 0.25rem; }
  .tool-card .desc { font-size: 0.78rem; color: var(--fg-muted); line-height: 1.4; }
  .tool-card.coming-soon { opacity: 0.55; cursor: default; }
  .tool-card.coming-soon:hover { transform: none; box-shadow: var(--card-shadow); border-color: var(--border); }
  .tool-card .badge {
    display: inline-block; font-size: 0.65rem; font-weight: 700; text-transform: uppercase;
    letter-spacing: 0.03em; background: var(--accent-soft); color: var(--accent);
    padding: 0.1rem 0.4rem; border-radius: 4px; margin-top: 0.5rem;
  }
  .tool-card.coming-soon .badge { background: #efece6; color: var(--fg-muted); }

  /* --- page editor --- */
  #view-page { max-width: 760px; margin: 0 auto; padding: 4rem 3rem 8rem; }
  #page-title {
    font-size: 2.1rem; font-weight: 700; outline: none; margin-bottom: 1.2rem;
    line-height: 1.3;
  }
  #page-title:empty::before { content: "Untitled"; color: #c9c2b8; }
  #save-indicator { font-size: 0.72rem; color: var(--fg-muted); height: 1em; margin-bottom: 0.5rem; }

  .block-wrap { display: flex; align-items: flex-start; gap: 0.4rem; margin: 0.15rem 0; }
  .block {
    flex: 1; outline: none; padding: 0.15rem 0.2rem; border-radius: 4px; line-height: 1.6;
    font-size: 0.98rem; min-height: 1.6em;
  }
  .block:hover { background: #faf9f7; }
  .block:focus { background: #f5f3ef; }
  .block:empty::before { content: attr(data-placeholder); color: #c9c2b8; }
  .block[data-type="heading"] { font-size: 1.4rem; font-weight: 700; margin-top: 0.4rem; }
  .block[data-type="quote"] { border-left: 3px solid var(--border); padding-left: 0.7rem; font-style: italic; color: #4a443d; }
  .block[data-type="code"] { font-family: ui-monospace, "SF Mono", monospace; font-size: 0.85rem;
                              background: #f5f3ef; padding: 0.5rem 0.7rem; border-radius: 6px; }
  .block[data-type="divider"] { border-top: 1px solid var(--border); min-height: 0; margin: 0.8rem 0; pointer-events: none; }
  .block-marker { width: 1.1rem; flex: 0 0 1.1rem; text-align: center; padding-top: 0.2rem; color: var(--fg-muted); }
  .block-marker input[type="checkbox"] { margin: 0; }
  #add-block {
    margin-top: 0.4rem; margin-left: 1.7rem; color: var(--fg-muted); font-size: 0.85rem;
    cursor: text; padding: 0.3rem 0.2rem;
  }
  #add-block:hover { background: #faf9f7; border-radius: 4px; }
  .editor-hint {
    font-size: 0.74rem; color: var(--fg-muted); margin: 0 0 1.6rem; padding: 0.5rem 0.7rem;
    background: #faf9f7; border-radius: 8px; line-height: 1.5;
  }

  /* --- notes / pdf / tools views --- */
  .simple-view { max-width: 760px; margin: 0 auto; padding: 3rem; }
  .simple-view h1 { font-size: 1.4rem; margin: 0 0 0.3rem; }
  .simple-view .subtitle { color: var(--fg-muted); font-size: 0.88rem; margin-bottom: 1.8rem; }
  .field-label { display: block; font-size: 0.8rem; color: var(--fg-muted); margin: 0.6rem 0 0.25rem; }
  .simple-view input[type="text"], .simple-view input:not([type]), .simple-view textarea {
    width: 100%; padding: 0.55rem 0.75rem; border: 1px solid var(--border); border-radius: 8px;
    font-size: 0.9rem; font-family: inherit;
  }
  .simple-view textarea { min-height: 5.5rem; resize: vertical; }
  .btn {
    margin-top: 0.9rem; padding: 0.55rem 1.1rem; border: none; border-radius: 8px;
    background: var(--accent); color: #fff; font-size: 0.86rem; font-weight: 600; cursor: pointer;
  }
  .btn:hover { opacity: 0.92; }
  .btn.secondary { background: #70655c; }
  .note-item {
    border: 1px solid var(--border); border-radius: 10px; padding: 0.8rem 1rem; margin-top: 0.7rem;
    font-size: 0.85rem; cursor: pointer; box-shadow: var(--card-shadow);
  }
  .note-item:hover { border-color: var(--accent); }
  .note-item .id { font-family: ui-monospace, monospace; font-size: 0.68rem; color: #8a7f74; margin-top: 0.35rem; }
  .status { font-size: 0.8rem; color: var(--fg-muted); margin-top: 0.6rem; min-height: 1.1em; }
  .status.err { color: #b23a3a; }
  canvas { border: 1px solid var(--border); border-radius: 8px; margin-top: 0.9rem; max-width: 100%; }

  .back-link { font-size: 0.82rem; color: var(--fg-muted); cursor: pointer; display: inline-flex;
               align-items: center; gap: 0.3rem; margin-bottom: 1.4rem; }
  .back-link:hover { color: var(--accent); }
</style>
</head>
<body>

<div id="sidebar">
  <div class="brand"><span class="brand-icon">📘</span><span>Notetrack</span></div>
  <nav class="nav">
    <button class="nav-item active" id="tab-home" onclick="switchTab('home')"><span class="nav-icon">🏠</span>Home</button>
    <button class="nav-item" id="tab-notes" onclick="switchTab('notes')"><span class="nav-icon">🗒️</span>Notes</button>
    <button class="nav-item" id="tab-pdf" onclick="switchTab('pdf')"><span class="nav-icon">📑</span>PDF Viewer</button>
    <button class="nav-item" id="tab-tools" onclick="switchTab('tools')"><span class="nav-icon">🧰</span>Tools</button>
  </nav>
  <div class="nav-section-label"><span>Pages</span><button class="icon-btn" onclick="createPage()" title="New page">＋</button></div>
  <div id="page-list"></div>
</div>

<div id="main">

  <div class="view visible" id="view-home">
    <h1>Welcome back</h1>
    <div class="subtitle">Pages, notes, PDFs and document tools, all in one place.</div>

    <div class="section-title">Recent pages</div>
    <div class="card-row" id="recent-pages-row">
      <div class="new-page-card" onclick="createPage()">＋ New page</div>
    </div>

    <div class="section-title">Tools</div>
    <div class="card-row" id="tools-row">
      <div class="tool-card" onclick="switchTab('tools')">
        <div class="icon">🖼️→📑</div>
        <div class="title">Images to PDF</div>
        <div class="desc">Combine one or more photos or images into a single PDF.</div>
        <div class="badge">Ready</div>
      </div>
      <div class="tool-card" onclick="switchTab('pdf')">
        <div class="icon">📑</div>
        <div class="title">PDF Viewer</div>
        <div class="desc">Open a PDF and view it page by page, rendered by a real PDF engine.</div>
        <div class="badge">Ready</div>
      </div>
      <div class="tool-card coming-soon" title="Not built yet">
        <div class="icon">🔗📄</div>
        <div class="title">Merge PDFs</div>
        <div class="desc">Combine multiple PDFs into one document.</div>
        <div class="badge">Coming soon</div>
      </div>
      <div class="tool-card coming-soon" title="Not built yet">
        <div class="icon">✂️📄</div>
        <div class="title">Split / Rotate PDF</div>
        <div class="desc">Reorder, rotate, or pull pages out of a PDF.</div>
        <div class="badge">Coming soon</div>
      </div>
      <div class="tool-card coming-soon" title="Not built yet">
        <div class="icon">📷→📝</div>
        <div class="title">Scan to PDF</div>
        <div class="desc">Photograph a document, auto-crop and straighten it, export as PDF.</div>
        <div class="badge">Coming soon</div>
      </div>
    </div>
  </div>

  <div class="view" id="view-page">
    <div class="back-link" onclick="switchTab('home')">← Home</div>
    <div id="save-indicator"></div>
    <div id="page-title" contenteditable="true" data-placeholder="Untitled"></div>
    <div class="editor-hint">Type <code>#&nbsp;</code> for a heading, <code>-&nbsp;</code> or <code>1.&nbsp;</code> for a list, <code>[]&nbsp;</code> for a checklist, <code>&gt;&nbsp;</code> for a quote, <code>```&nbsp;</code> for code, <code>---</code> for a divider — Notion-style. Enter adds a block, Backspace on an empty block removes it.</div>
    <div id="blocks"></div>
    <div id="add-block">Click to add a block…</div>
  </div>

  <div class="view simple-view" id="view-notes">
    <div class="back-link" onclick="switchTab('home')">← Home</div>
    <h1>Notes</h1>
    <div class="subtitle">Quick, flat notes — separate from pages, searchable by content.</div>
    <label class="field-label">Title</label><input type="text" id="note-title" placeholder="Title">
    <label class="field-label">Body</label><textarea id="note-body" placeholder="Write something…"></textarea>
    <button class="btn" onclick="createNote()">Create note</button>
    <label class="field-label" style="margin-top:1.4rem">Search notes</label>
    <div style="display:flex; gap:0.5rem;"><input type="text" id="note-query" placeholder="search term"><button class="btn secondary" onclick="searchNotes()">Search</button></div>
    <div id="note-list"></div>
    <div class="status" id="note-status"></div>
  </div>

  <div class="view simple-view" id="view-pdf">
    <div class="back-link" onclick="switchTab('home')">← Home</div>
    <h1>PDF Viewer</h1>
    <div class="subtitle">Open any PDF and view its pages, rendered by the real PDF engine.</div>
    <input type="file" id="pdf-file" accept="application/pdf">
    <button class="btn" onclick="uploadPdf()">Open &amp; view page 1</button>
    <div id="pdf-info" class="status"></div>
    <canvas id="pdf-canvas" width="500" height="650"></canvas>
    <div class="status" id="pdf-status"></div>
  </div>

  <div class="view simple-view" id="view-tools">
    <div class="back-link" onclick="switchTab('home')">← Home</div>
    <h1>Images to PDF</h1>
    <div class="subtitle">Pick one or more images — get back one real, multi-page PDF, built by this server's own PDF engine.</div>
    <input type="file" id="tool-images-file" accept="image/*" multiple>
    <button class="btn" onclick="imagesToPdf()">Convert to PDF</button>
    <div class="status" id="tool-images-status"></div>
  </div>

</div>

<script>
// ---------------------------------------------------------------------
// state
//
// pages is a SESSION-local cache, not server state: there is no "list
// every page" endpoint in the API yet (pages are opened by id or found by
// search), so this is this browser's own memory of what it has created or
// opened, persisted to localStorage only so a reload does not lose it.
// ---------------------------------------------------------------------
let pages = JSON.parse(localStorage.getItem('notetrack.pages') || '[]');
let currentPageId = null;
let currentBlocks = [];   // [{id, type, text, heading_level, checked, language}]
let saveTimers = {};      // blockId -> setTimeout handle, debounced autosave

function persistPageList() {
  localStorage.setItem('notetrack.pages', JSON.stringify(pages));
}

async function api(path, opts) {
  const res = await fetch(path, opts);
  const text = await res.text();
  let body = null;
  if (text) { try { body = JSON.parse(text); } catch (e) { body = text; } }
  if (!res.ok) {
    const msg = (body && body.error && body.error.message) ? body.error.message : ('HTTP ' + res.status);
    throw new Error(msg);
  }
  return body;
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

// ---------------------------------------------------------------------
// navigation
// ---------------------------------------------------------------------
function switchTab(name) {
  currentPageId = null;  // leaving the editor; a sidebar page click sets this again
  for (const id of ['tab-home', 'tab-notes', 'tab-pdf', 'tab-tools']) {
    document.getElementById(id).classList.toggle('active', id === 'tab-' + name);
  }
  for (const id of ['view-home', 'view-page', 'view-notes', 'view-pdf', 'view-tools']) {
    document.getElementById(id).classList.toggle('visible', id === 'view-' + name);
  }
  renderPageList();
  if (name === 'home') renderRecentPages();
}

// ---------------------------------------------------------------------
// sidebar / pages list / home dashboard
// ---------------------------------------------------------------------
function renderPageList() {
  const el = document.getElementById('page-list');
  el.innerHTML = '';
  if (pages.length === 0) {
    el.innerHTML = '<div class="empty-pages-note">No pages yet — click ＋ to start one.</div>';
    return;
  }
  for (const p of pages) {
    const row = document.createElement('div');
    row.className = 'page-item' + (p.id === currentPageId ? ' selected' : '');
    row.innerHTML = '<span class="title">📄 ' + escapeHtml(p.title || 'Untitled') + '</span><span class="del" title="Delete">&times;</span>';
    row.querySelector('.title').onclick = () => openPage(p.id);
    row.querySelector('.del').onclick = (e) => { e.stopPropagation(); deletePage(p.id); };
    el.appendChild(row);
  }
}

function renderRecentPages() {
  const row = document.getElementById('recent-pages-row');
  row.innerHTML = '';
  for (const p of pages.slice(0, 7)) {
    const card = document.createElement('div');
    card.className = 'recent-card';
    card.innerHTML = '<div class="icon">📄</div><div class="title">' + escapeHtml(p.title || 'Untitled') + '</div>';
    card.onclick = () => openPage(p.id);
    row.appendChild(card);
  }
  const newCard = document.createElement('div');
  newCard.className = 'new-page-card';
  newCard.textContent = '＋ New page';
  newCard.onclick = createPage;
  row.appendChild(newCard);
}

async function createPage() {
  try {
    const page = await api('/documents', { method: 'POST', body: JSON.stringify({ title: '' }) });
    pages.unshift({ id: page.id, title: page.title });
    persistPageList();
    await openPage(page.id);
    document.getElementById('page-title').focus();
  } catch (e) { alert('Could not create page: ' + e.message); }
}

async function deletePage(id) {
  if (!confirm('Delete this page and all its blocks?')) return;
  try {
    await api('/documents/' + id, { method: 'DELETE' });
  } catch (e) { /* already gone server-side is fine for the sidebar's purposes */ }
  pages = pages.filter(p => p.id !== id);
  persistPageList();
  if (currentPageId === id) { switchTab('home'); return; }
  renderPageList();
}

async function openPage(pageId) {
  try {
    const page = await api('/documents/' + pageId);
    currentPageId = pageId;
    currentBlocks = page.blocks;
    for (const elId of ['tab-home', 'tab-notes', 'tab-pdf', 'tab-tools']) {
      document.getElementById(elId).classList.remove('active');
    }
    for (const elId of ['view-home', 'view-page', 'view-notes', 'view-pdf', 'view-tools']) {
      document.getElementById(elId).classList.toggle('visible', elId === 'view-page');
    }
    document.getElementById('page-title').textContent = page.title;
    renderBlocks();
    renderPageList();
    const idx = pages.findIndex(p => p.id === pageId);
    if (idx >= 0) { pages[idx].title = page.title; persistPageList(); }
  } catch (e) { alert('Could not open page: ' + e.message); }
}

function setSaveIndicator(text) {
  const el = document.getElementById('save-indicator');
  el.textContent = text;
  if (text) setTimeout(() => { if (el.textContent === text) el.textContent = ''; }, 1500);
}

document.getElementById('page-title').addEventListener('blur', async () => {
  if (!currentPageId) return;
  const title = document.getElementById('page-title').textContent;
  try {
    await api('/documents/' + currentPageId, { method: 'PATCH', body: JSON.stringify({ title }) });
    const idx = pages.findIndex(p => p.id === currentPageId);
    if (idx >= 0) { pages[idx].title = title; persistPageList(); renderPageList(); }
    setSaveIndicator('Saved');
  } catch (e) { setSaveIndicator('Could not save title: ' + e.message); }
});
document.getElementById('page-title').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();
    const first = document.querySelector('#blocks .block');
    if (first) { first.focus(); return; }
    if (currentPageId) insertBlockAfterOptimistic(null, 'paragraph');
  }
});

// ---------------------------------------------------------------------
// blocks
// ---------------------------------------------------------------------
const TYPE_PLACEHOLDER = {
  paragraph: "Type something…", heading: "Heading", quote: "Quote",
  code: "Code", bulleted_list_item: "List item", numbered_list_item: "List item",
  checklist_item: "To-do", divider: ""
};

function renderBlocks() {
  const el = document.getElementById('blocks');
  el.innerHTML = '';
  currentBlocks.forEach((b, i) => el.appendChild(renderBlockRow(b, i)));
}

// Updates only numbered-list marker text, by walking the DOM in its
// current order — never touches a `.block` div itself, so it never steals
// focus from whichever block the user is actively typing in. Needed after
// a targeted insert/delete (see insertBlockAfterOptimistic,
// deleteBlockAndFocusPrev below) because those deliberately do NOT call
// renderBlocks() for the common case, and a numbered list's numbers must
// still renumber when an item is added or removed from the middle of one.
function renumberMarkers() {
  let run = 0;
  for (const wrap of document.querySelectorAll('#blocks .block-wrap')) {
    const b = currentBlocks.find(x => x.id === wrap.dataset.blockId);
    if (!b) continue;
    if (b.type === 'numbered_list_item') {
      run += 1;
      const marker = wrap.querySelector('.block-marker');
      if (marker && !marker.querySelector('input')) marker.textContent = run + '.';
    } else {
      run = 0;
    }
  }
}

// Rebuilds and refocuses ONE block's row in place — used when a single
// block's TYPE changes (a "/ "-style shortcut, or exiting a list on an
// empty Enter). Replacing just that one wrap, rather than calling
// renderBlocks() for every block, is the same fix insertBlockAfterOptimistic
// applies to inserts: touching only the block that actually changed is
// what keeps a focus-and-immediately-type sequence from racing a full
// rebuild of unrelated blocks.
function replaceBlockRow(id) {
  const b = currentBlocks.find(x => x.id === id);
  const oldWrap = document.querySelector('[data-block-id="' + id + '"]');
  if (!b || !oldWrap) return;
  const index = currentBlocks.findIndex(x => x.id === id);
  const newWrap = renderBlockRow(b, index);
  oldWrap.replaceWith(newWrap);
  renumberMarkers();
  const row = newWrap.querySelector('.block');
  if (row) row.focus();
}

function renderBlockRow(b, index) {
  const wrap = document.createElement('div');
  wrap.className = 'block-wrap';
  wrap.dataset.blockId = b.id;

  const marker = document.createElement('div');
  marker.className = 'block-marker';
  if (b.type === 'bulleted_list_item') marker.textContent = '•';
  else if (b.type === 'numbered_list_item') marker.textContent = (countSameTypeBefore(index, 'numbered_list_item') + 1) + '.';
  else if (b.type === 'checklist_item') {
    const cb = document.createElement('input');
    cb.type = 'checkbox'; cb.checked = !!b.checked;
    cb.onchange = async () => {
      const realId = await resolveBlockId(b.id);
      if (!realId) return;
      b.checked = cb.checked;
      updateBlock(realId, b);
    };
    marker.appendChild(cb);
  }

  const div = document.createElement('div');
  div.className = 'block';
  div.contentEditable = 'true';
  div.dataset.type = b.type;
  div.dataset.placeholder = TYPE_PLACEHOLDER[b.type] || '';
  div.textContent = b.text || '';

  div.addEventListener('input', () => onBlockInput(b.id, div));
  div.addEventListener('keydown', (e) => onBlockKeydown(e, b.id, div));
  div.addEventListener('blur', () => flushBlockSave(b.id, div));

  wrap.appendChild(marker);
  wrap.appendChild(div);
  return wrap;
}

function countSameTypeBefore(index, type) {
  let n = 0;
  for (let i = 0; i < index; i++) {
    if (currentBlocks[i].type === type) n++; else n = 0;
  }
  return n;
}

const SHORTCUTS = [
  { re: /^```\s$/, type: 'code', strip: true },
  { re: /^#\s$/, type: 'heading', strip: true },
  { re: /^-\s$/, type: 'bulleted_list_item', strip: true },
  { re: /^\*\s$/, type: 'bulleted_list_item', strip: true },
  { re: /^1\.\s$/, type: 'numbered_list_item', strip: true },
  { re: /^\[\]\s$/, type: 'checklist_item', strip: true },
  { re: /^>\s$/, type: 'quote', strip: true },
  { re: /^---$/, type: 'divider', strip: true }
];

// A block created optimistically (see insertBlockAfterOptimistic) carries a
// client-side 'tmp-' id until its real id comes back from the server. A
// save triggered WHILE that creation is still in flight needs to wait for
// it — pendingCreation is that wait point. A save triggered by the
// debounce timer AFTER creation already finished needs a different thing:
// the timer closure captured the tmp- id back when it was scheduled, and
// by the time it fires pendingCreation[tempId] has already been deleted
// (the whole point of a transient "in flight" map), so there would be no
// way left to find the real id — tempIdToRealId is that permanent record,
// the actual bug this fixes: typing into a freshly-continued list item
// looked correct on screen but silently never reached the server, because
// resolveBlockId had no way to recognise an ALREADY-reconciled tmp- id and
// gave up instead of returning the real one.
const pendingCreation = {};    // tempId -> Promise<realId | null>, while creation is in flight
const tempIdToRealId = {};     // tempId -> realId, kept for the block's whole remaining life

async function resolveBlockId(id) {
  if (!id.startsWith('tmp-')) return id;
  if (tempIdToRealId[id]) return tempIdToRealId[id];
  if (pendingCreation[id]) {
    try { return await pendingCreation[id]; } catch (e) { return null; }
  }
  return null;  // an unknown tmp- id: creation failed and was already cleaned up
}

function onBlockInput(id, div) {
  const text = div.textContent;
  for (const s of SHORTCUTS) {
    if (s.re.test(text)) {
      const b = currentBlocks.find(x => x.id === id);
      if (b) { b.type = s.type; b.text = ''; }
      if (s.type === 'divider') { flushBlockSave(id, div); }
      replaceBlockRow(id);
      return;
    }
  }
  clearTimeout(saveTimers[id]);
  saveTimers[id] = setTimeout(() => flushBlockSave(id, div), 500);
}

async function flushBlockSave(id, div) {
  clearTimeout(saveTimers[id]);
  const realId = await resolveBlockId(id);
  if (!realId) return;  // the optimistic creation failed; nothing to save
  const b = currentBlocks.find(x => x.id === realId);
  if (!b) return;
  b.text = div.textContent;
  b.type = div.dataset.type;
  updateBlock(realId, b);
}

async function updateBlock(id, b) {
  try {
    await api('/documents/' + currentPageId + '/blocks/' + id, {
      method: 'PATCH',
      body: JSON.stringify({
        type: b.type, text: b.text || '', heading_level: b.heading_level || 1,
        checked: !!b.checked, language: b.language || ''
      })
    });
    setSaveIndicator('Saved');
  } catch (e) { setSaveIndicator('Could not save: ' + e.message); }
}

const LIST_TYPES = ['bulleted_list_item', 'numbered_list_item', 'checklist_item'];

async function onBlockKeydown(e, id, div) {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    const currentType = div.dataset.type;
    const isEmpty = div.textContent.trim() === '';

    if (LIST_TYPES.includes(currentType) && isEmpty) {
      // Enter on an EMPTY list item exits the list (converts this block to
      // a paragraph) instead of adding yet another empty item — the same
      // behaviour real Notion has, and the one most people expect.
      const b = currentBlocks.find(x => x.id === id);
      if (b) { b.type = 'paragraph'; b.text = ''; }
      replaceBlockRow(id);
      const realId = await resolveBlockId(id);
      if (realId && b) updateBlock(realId, b);
      return;
    }

    flushBlockSave(id, div);
    // Enter from WITHIN a list item continues the same list type; from
    // anything else it starts a fresh paragraph.
    insertBlockAfterOptimistic(id, LIST_TYPES.includes(currentType) ? currentType : 'paragraph');
    return;
  }
  if (e.key === 'Backspace' && div.textContent === '') {
    e.preventDefault();
    deleteBlockAndFocusPrev(id);
    return;
  }
}

// Inserts ONE new block-wrap into the live DOM at the right position,
// rather than calling renderBlocks() (which tears down and recreates
// EVERY block). That full-rebuild approach was tried first and caused a
// real, reproducible bug: focusing the freshly rebuilt node right after a
// rebuild raced the browser's own focus/selection settling on rapid
// keystrokes, so text typed immediately after Enter sometimes landed in
// no block at all. A targeted insert never touches any other block's DOM
// node, so there is nothing for that race to happen to. The real creation
// happens in the background; pendingCreation/tempIdToRealId let a save
// that starts before or after it resolves still find the real id.
function insertBlockAfterOptimistic(afterId, type) {
  const index = afterId == null ? currentBlocks.length
                                 : currentBlocks.findIndex(b => b.id === afterId) + 1;
  const localId = 'tmp-' + Math.random().toString(36).slice(2);
  const newBlock = { id: localId, type, text: '', heading_level: 1, checked: false, language: '' };
  currentBlocks.splice(index, 0, newBlock);

  const container = document.getElementById('blocks');
  const referenceNode = container.children[index] || null;
  const newRow = renderBlockRow(newBlock, index);
  container.insertBefore(newRow, referenceNode);
  renumberMarkers();

  const row = newRow.querySelector('.block');
  if (row) row.focus();

  pendingCreation[localId] = (async () => {
    try {
      const result = await api('/documents/' + currentPageId + '/blocks', {
        method: 'POST', body: JSON.stringify({ type, text: '', position: index })
      });
      tempIdToRealId[localId] = result.id;
      const b = currentBlocks.find(x => x.id === localId);
      if (b) {
        b.id = result.id;
        const wrap = document.querySelector('[data-block-id="' + localId + '"]');
        if (wrap) wrap.dataset.blockId = result.id;
      }
      return result.id;
    } catch (e) {
      currentBlocks = currentBlocks.filter(x => x.id !== localId);
      renderBlocks();
      setSaveIndicator('Could not add block: ' + e.message);
      return null;
    } finally {
      delete pendingCreation[localId];
    }
  })();
}

document.getElementById('add-block').addEventListener('click', () => {
  if (!currentPageId) return;
  const lastId = currentBlocks.length ? currentBlocks[currentBlocks.length - 1].id : null;
  insertBlockAfterOptimistic(lastId, 'paragraph');
});

async function deleteBlockAndFocusPrev(id) {
  const realId = await resolveBlockId(id);
  if (!realId) return;
  const index = currentBlocks.findIndex(b => b.id === realId);
  if (index < 0) return;
  try {
    await api('/documents/' + currentPageId + '/blocks/' + realId, { method: 'DELETE' });
  } catch (e) { setSaveIndicator('Could not delete: ' + e.message); return; }
  currentBlocks.splice(index, 1);
  const wrap = document.querySelector('[data-block-id="' + realId + '"]');
  if (wrap) wrap.remove();
  renumberMarkers();
  if (index > 0) {
    const prevId = currentBlocks[index - 1].id;
    const row = document.querySelector('[data-block-id="' + prevId + '"] .block');
    if (row) { row.focus(); placeCursorAtEnd(row); }
  }
}

function placeCursorAtEnd(el) {
  const range = document.createRange();
  range.selectNodeContents(el);
  range.collapse(false);
  const sel = window.getSelection();
  sel.removeAllRanges();
  sel.addRange(range);
}

// ---------------------------------------------------------------------
// notes
// ---------------------------------------------------------------------
const notes = [];
function setStatus(id, text, isErr) {
  const el = document.getElementById(id);
  el.textContent = text || '';
  el.className = 'status' + (isErr ? ' err' : '');
}
function renderNoteList() {
  const el = document.getElementById('note-list');
  el.innerHTML = '';
  for (const n of notes) {
    const div = document.createElement('div');
    div.className = 'note-item';
    div.innerHTML = '<strong>' + escapeHtml(n.title || '(untitled)') + '</strong><br>' +
                    escapeHtml((n.snippet || n.body || '').slice(0, 140)) +
                    '<div class="id">' + n.id + '</div>';
    div.onclick = async () => {
      try {
        const note = await api('/notes/' + n.id);
        document.getElementById('note-title').value = note.title;
        document.getElementById('note-body').value = note.body;
      } catch (e) { setStatus('note-status', e.message, true); }
    };
    el.appendChild(div);
  }
}
async function createNote() {
  const title = document.getElementById('note-title').value;
  const body  = document.getElementById('note-body').value;
  try {
    const note = await api('/notes', { method: 'POST', body: JSON.stringify({ title, body }) });
    notes.unshift(note);
    renderNoteList();
    document.getElementById('note-title').value = '';
    document.getElementById('note-body').value = '';
    setStatus('note-status', 'Created ' + note.id);
  } catch (e) { setStatus('note-status', e.message, true); }
}
async function searchNotes() {
  const q = document.getElementById('note-query').value;
  if (!q) { setStatus('note-status', 'type something to search for', true); return; }
  try {
    const hits = await api('/notes?q=' + encodeURIComponent(q));
    notes.length = 0;
    for (const h of hits) notes.push(h);
    renderNoteList();
    setStatus('note-status', hits.length + ' result(s)');
  } catch (e) { setStatus('note-status', e.message, true); }
}

// ---------------------------------------------------------------------
// pdf viewer
// ---------------------------------------------------------------------
async function uploadPdf() {
  const input = document.getElementById('pdf-file');
  if (!input.files.length) { setStatus('pdf-status', 'choose a PDF file first', true); return; }
  try {
    const bytes = await input.files[0].arrayBuffer();
    const uploadRes = await fetch('/pdf', { method: 'POST', body: bytes });
    const upload = await uploadRes.json();
    if (!uploadRes.ok) throw new Error((upload.error && upload.error.message) || 'upload failed');

    const sizeRes = await fetch('/pdf/' + upload.id + '/page/0/size');
    const size = await sizeRes.json();
    document.getElementById('pdf-info').textContent =
      upload.page_count + ' page(s), page 1 is ' + size.width_points + 'x' + size.height_points + ' pt';

    const canvas = document.getElementById('pdf-canvas');
    const scale = canvas.width / size.width_points;
    const height = Math.round(size.height_points * scale);
    canvas.height = height;

    const renderRes = await fetch('/pdf/' + upload.id + '/page/0/render?width=' + canvas.width + '&height=' + height, { method: 'POST' });
    const pixels = new Uint8ClampedArray(await renderRes.arrayBuffer());
    const ctx = canvas.getContext('2d');
    const imageData = new ImageData(pixels, canvas.width, height);
    ctx.putImageData(imageData, 0, 0);
    setStatus('pdf-status', 'Rendered.');
  } catch (e) { setStatus('pdf-status', e.message, true); }
}

// ---------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------
async function imagesToPdf() {
  const input = document.getElementById('tool-images-file');
  if (!input.files.length) { setStatus('tool-images-status', 'choose at least one image first', true); return; }
  try {
    const form = new FormData();
    for (const file of input.files) { form.append('file', file, file.name); }

    const res = await fetch('/tools/images-to-pdf', { method: 'POST', body: form });
    if (!res.ok) {
      const body = await res.json().catch(() => null);
      throw new Error((body && body.error && body.error.message) || ('HTTP ' + res.status));
    }
    const pdfBlob = await res.blob();

    // Trigger a real browser download — the server returned actual PDF
    // bytes, not a link to one, so this is the client's own job.
    const url = URL.createObjectURL(pdfBlob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'images.pdf';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);

    setStatus('tool-images-status', 'Converted ' + input.files.length + ' image(s) into a ' +
                                       pdfBlob.size + '-byte PDF. Downloaded.');
  } catch (e) { setStatus('tool-images-status', e.message, true); }
}

// ---------------------------------------------------------------------
// boot
// ---------------------------------------------------------------------
renderPageList();
renderRecentPages();
</script>
</body>
</html>
)HTML";

}  // namespace

void register_ui_routes(transport::HttpServer& server) {
    server.get("/", [](const transport::Request&, transport::Response& res) {
        res.status       = 200;
        res.content_type = "text/html; charset=utf-8";
        res.body         = kPage;
    });
}

}  // namespace uapp::api
