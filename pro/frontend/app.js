const STORAGE_KEY = "p2p_nodes";

function apiBase() {
  return document.getElementById("apiBase").value.trim();
}

function setBox(id, text) {
  document.getElementById(id).textContent = text;
}

function buildQuery(params) {
  const esc = encodeURIComponent;
  return Object.keys(params)
    .filter((k) => params[k] !== "" && params[k] !== undefined && params[k] !== null)
    .map((k) => `${esc(k)}=${esc(params[k])}`)
    .join("&");
}

async function fetchJson(url, options) {
  const res = await fetch(url, options);
  const text = await res.text();
  try {
    return JSON.parse(text);
  } catch (e) {
    return { raw: text };
  }
}

function loadNodes() {
  const raw = localStorage.getItem(STORAGE_KEY);
  if (!raw) {
    return ["http://127.0.0.1:8080"];
  }
  try {
    const arr = JSON.parse(raw);
    return Array.isArray(arr) && arr.length > 0 ? arr : ["http://127.0.0.1:8080"];
  } catch (e) {
    return ["http://127.0.0.1:8080"];
  }
}

function saveNodes(nodes) {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(nodes));
}

function setActiveNode(url) {
  document.getElementById("apiBase").value = url;
  renderNodes();
}

async function fetchStatus(url) {
  try {
    return await fetchJson(`${url}/status`);
  } catch (e) {
    return { error: "unreachable" };
  }
}

async function renderNodes() {
  const list = document.getElementById("nodeList");
  const nodes = loadNodes();
  const active = apiBase();
  list.innerHTML = "";
  for (const url of nodes) {
    const item = document.createElement("div");
    item.className = "node-item" + (url === active ? " active" : "");
    const status = await fetchStatus(url);
    const title = document.createElement("div");
    title.className = "node-title";
    title.textContent = url;
    const meta = document.createElement("div");
    meta.className = "node-meta";
    if (status && !status.error) {
      meta.textContent = `node_id=${status.node_id || "-"} self_addr=${status.self_addr || "-"}`;
    } else {
      meta.textContent = "状态：不可达";
    }
    const actions = document.createElement("div");
    actions.className = "node-actions";
    const btnUse = document.createElement("button");
    btnUse.textContent = "使用";
    btnUse.onclick = () => setActiveNode(url);
    const btnRemove = document.createElement("button");
    btnRemove.textContent = "删除";
    btnRemove.onclick = () => {
      const next = nodes.filter((n) => n !== url);
      saveNodes(next);
      if (url === active && next.length > 0) {
        setActiveNode(next[0]);
      } else {
        renderNodes();
      }
    };
    actions.appendChild(btnUse);
    actions.appendChild(btnRemove);
    item.appendChild(title);
    item.appendChild(meta);
    item.appendChild(actions);
    list.appendChild(item);
  }
}

document.getElementById("btnAddNode").addEventListener("click", () => {
  const input = document.getElementById("nodeInput");
  const url = input.value.trim();
  if (!url) {
    return;
  }
  const nodes = loadNodes();
  if (!nodes.includes(url)) {
    nodes.push(url);
    saveNodes(nodes);
  }
  input.value = "";
  renderNodes();
});

document.getElementById("btnRefreshAll").addEventListener("click", () => {
  renderNodes();
});

document.getElementById("btnStatus").addEventListener("click", async () => {
  const url = `${apiBase()}/status`;
  const data = await fetchJson(url);
  setBox("statusBox", JSON.stringify(data, null, 2));
});

document.getElementById("btnUpload").addEventListener("click", async () => {
  const file = document.getElementById("uploadFile").value.trim();
  const replica = document.getElementById("uploadReplica").value.trim();
  const manifest = document.getElementById("uploadManifest").value.trim();
  const q = buildQuery({ file, replica, manifest_out: manifest });
  const url = `${apiBase()}/upload?${q}`;
  const data = await fetchJson(url, { method: "POST" });
  setBox("uploadBox", JSON.stringify(data, null, 2));
});

document.getElementById("btnDownload").addEventListener("click", async () => {
  const manifest = document.getElementById("downloadManifest").value.trim();
  const out = document.getElementById("downloadOut").value.trim();
  const peer = document.getElementById("downloadPeer").value.trim();
  const strategy = document.getElementById("downloadStrategy").value.trim();
  const secure = document.getElementById("downloadSecure").value.trim();
  const q = buildQuery({ manifest, out, peer, strategy, secure });
  const url = `${apiBase()}/download?${q}`;
  const data = await fetchJson(url, { method: "POST" });
  setBox("downloadBox", JSON.stringify(data, null, 2));
});

document.getElementById("btnStats").addEventListener("click", async () => {
  const type = document.getElementById("statsType").value.trim();
  const owner = document.getElementById("statsOwner").value.trim();
  const limit = document.getElementById("statsLimit").value.trim();
  const q = buildQuery({ type, owner, limit });
  const url = `${apiBase()}/stats?${q}`;
  const data = await fetchJson(url);
  setBox("statsBox", JSON.stringify(data, null, 2));
});

window.addEventListener("load", () => {
  const nodes = loadNodes();
  document.getElementById("apiBase").value = nodes[0];
  renderNodes();
});
