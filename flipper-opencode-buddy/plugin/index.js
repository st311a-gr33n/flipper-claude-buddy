import { connect } from "net";
import { existsSync, readFileSync, writeFileSync, unlinkSync, mkdirSync } from "fs";
import { resolve, dirname } from "path";
import { fileURLToPath } from "url";

const SOCKET = "/tmp/claude-flipper-bridge.sock";
const PIDFILE = "/tmp/claude-flipper-bridge.pid";
const LOG = "/tmp/claude-flipper-bridge.log";
const STATS = "/tmp/claude-flipper-turn-stats.json";
const REFCOUNT = "/tmp/claude-flipper-bridge.refcount";
const SKIP_STOP_FLAG = "/tmp/claude-flipper-skip-stop.flag";

const __dirname = dirname(fileURLToPath(import.meta.url));
const PLUGIN_ROOT = resolve(__dirname, "..");
const BRIDGE_DIR = resolve(PLUGIN_ROOT, "host-bridge");
const SCRIPTS_DIR = resolve(PLUGIN_ROOT, "scripts");
const REPO_ROOT = resolve(PLUGIN_ROOT, "..");
const MAIN_BRIDGE_DIR = resolve(REPO_ROOT, "plugin", "host-bridge");
const PLUGIN_DATA = resolve("/tmp", "flipper-opencode-buddy", "data");
const VENV_DIR = resolve(PLUGIN_DATA, "venv");
const BT_NAME_CACHE = resolve(PLUGIN_DATA, "bt_name");

function send(msg) {
  return new Promise((resolve, reject) => {
    try {
      const client = connect(SOCKET, () => {
        client.write(JSON.stringify(msg));
        client.end();
      });
      let data = "";
      client.on("data", (chunk) => (data += chunk));
      client.on("end", () => {
        try {
          resolve(data ? JSON.parse(data) : null);
        } catch {
          resolve(null);
        }
      });
      client.on("error", (err) => reject(err));
      setTimeout(() => {
        client.destroy();
        reject(new Error("timeout"));
      }, 5000);
    } catch {
      resolve(null);
    }
  });
}

function sendNotify(sound, text, subtext = "", vibro = true) {
  return send({ action: "notify", sound, vibro, text, subtext: subtext.slice(0, 21) });
}

function sendDisplay(text, subtext = "") {
  return send({ action: "display", text: text.slice(0, 21), subtext: subtext.slice(0, 21) });
}

function readStat() {
  try {
    if (existsSync(STATS)) return JSON.parse(readFileSync(STATS, "utf8"));
  } catch {}
  return {};
}

function writeStat(stats) {
  try {
    writeFileSync(STATS, JSON.stringify(stats));
  } catch {}
}

const TOOL_SOUNDS = [
  [new Set(["Edit", "Write", "NotebookEdit"]), "enter"],
  [new Set(["Bash"]), "cmd"],
  [new Set(["WebFetch", "WebSearch"]), "alert"],
  [new Set(["Read"]), "enter"],
  [new Set(["Glob", "Grep"]), null],
];

function soundForTool(name) {
  for (const [tools, sound] of TOOL_SOUNDS) {
    if (tools.has(name)) return sound;
  }
  return null;
}

function toolDetail(name, input) {
  if (name === "Bash") return (input.command || "").slice(0, 21);
  if (["Edit", "Write", "Read"].includes(name)) {
    const p = input.filePath || "";
    return p.split("/").pop().slice(0, 21);
  }
  if (["WebFetch", "WebSearch"].includes(name)) {
    const v = input.url || input.query || "";
    return v.replace(/^https?:\/\//, "").slice(0, 21);
  }
  return "";
}

export const FlipperOpenCodeBuddy = async ({ project, $, directory }) => {
  let bridgeStarted = false;

  async function ensureBridge() {
    if (bridgeStarted && existsSync(SOCKET)) return true;
    if (existsSync(SOCKET)) {
      bridgeStarted = true;
      return true;
    }
    mkdirSync(PLUGIN_DATA, { recursive: true });

    const hashCmd = await $`cat ${MAIN_BRIDGE_DIR}/pyproject.toml ${MAIN_BRIDGE_DIR}/bridge/*.py 2>/dev/null | md5sum 2>/dev/null || echo none`;
    const currentHash = hashCmd.text().trim().split(" ")[0] || "none";
    const markerFile = resolve(PLUGIN_DATA, ".installed-hash");
    const markerHash = existsSync(markerFile) ? readFileSync(markerFile, "utf8").trim() : "";

    const needsInstall = !existsSync(resolve(VENV_DIR, "bin", "python")) || markerHash !== currentHash;

    if (needsInstall) {
      await $`python3 -m venv ${VENV_DIR}`;
      await $`${VENV_DIR}/bin/pip install -q --force-reinstall ${MAIN_BRIDGE_DIR}`;
      writeFileSync(markerFile, currentHash);
    }

    const transport = process.env.FLIPPER_TRANSPORT || "auto";
    const btName = process.env.FLIPPER_BT_NAME || (
      existsSync(BT_NAME_CACHE) ? readFileSync(BT_NAME_CACHE, "utf8").trim() : ""
    );

    const env = { ...process.env };
    if (process.env.FLIPPER_SERIAL_PORT) env.FLIPPER_SERIAL_PORT = process.env.FLIPPER_SERIAL_PORT;
    if (btName) env.FLIPPER_BT_NAME = btName;
    env.FLIPPER_PLUGIN_DATA = PLUGIN_DATA;
    if (directory) env.FLIPPER_PROJECT_DIR = directory;

    const args = [`${VENV_DIR}/bin/python`, "-m", "bridge"];
    if (transport) args.push("--transport", transport);

    const proc = Bun.spawn(args, {
      env,
      cwd: BRIDGE_DIR,
      stdout: { path: LOG, append: true },
      stderr: { path: LOG, append: true },
    });
    writeFileSync(PIDFILE, String(proc.pid));
    bridgeStarted = true;

    for (let i = 0; i < 30; i++) {
      if (existsSync(SOCKET)) return true;
      await new Promise((r) => setTimeout(r, 100));
    }
    return existsSync(SOCKET);
  }

  async function registerTarget() {
    if (!existsSync(SOCKET)) return;
    try {
      await $`python3 ${SCRIPTS_DIR}/session-target.py register_target ${SOCKET}`;
    } catch {}
  }

  async function releaseTarget() {
    if (!existsSync(SOCKET)) return;
    try {
      await $`python3 ${SCRIPTS_DIR}/session-target.py release_target ${SOCKET}`;
    } catch {}
  }

  async function stopBridge() {
    if (existsSync(PIDFILE)) {
      const pid = readFileSync(PIDFILE, "utf8").trim();
      try {
        process.kill(parseInt(pid), "SIGTERM");
      } catch {}
      try { unlinkSync(PIDFILE); } catch {}
    }
  }

  return {
    "session.created": async (input) => {
      const source = input.source || "";
      const subtext = { startup: "New session", resume: "Resumed", clear: "After clear", compact: "After compaction" }[source] || (input.model || "").slice(0, 21) || "Connected";

      const ok = await ensureBridge();
      if (!ok) return;

      await registerTarget();

      const refcount = (parseInt(existsSync(REFCOUNT) ? readFileSync(REFCOUNT, "utf8").trim() : "0") || 0) + 1;
      writeFileSync(REFCOUNT, String(refcount));

      try {
        await send({ action: "opencode_connect", project_dir: directory || "" });
      } catch {}

      try {
        await sendNotify("connect", "OpenCode", subtext);
      } catch {}
    },

    "session.deleted": async (input) => {
      if (!existsSync(SOCKET)) return;

      const reason = input.reason || "Disconnected";
      const reasonLabel = {
        clear: "Cleared",
        resume: "Switched session",
        logout: "Logged out",
      }[reason] || reason.slice(0, 21) || "Disconnected";

      await releaseTarget();

      try {
        await send({ action: "opencode_disconnect" });
      } catch {}

      const count = (parseInt(existsSync(REFCOUNT) ? readFileSync(REFCOUNT, "utf8").trim() : "1") || 1) - 1;
      writeFileSync(REFCOUNT, String(Math.max(0, count)));

      if (count <= 0) {
        try {
          await sendNotify("session_end", "Session End", reasonLabel);
        } catch {}
        await new Promise((r) => setTimeout(r, 500));
        await stopBridge();
        try { unlinkSync(REFCOUNT); } catch {}
      }
    },

    "tool.execute.before": async (input, output) => {
      if (!existsSync(SOCKET)) return;
      const name = input.tool || "";
      const detail = toolDetail(name, input);
      try {
        await sendDisplay(name, detail);
      } catch {}
    },

    "tool.execute.after": async (input, output) => {
      if (!existsSync(SOCKET)) return;
      const name = input.tool || "";

      if (name === "Bash") {
        const cmd = (input.command || "").trim();
        if (cmd.includes(SOCKET)) {
          try { writeFileSync(SKIP_STOP_FLAG, ""); } catch {}
          return;
        }
      }

      const stats = readStat();
      stats[name] = (stats[name] || 0) + 1;
      writeStat(stats);

      if (output && output.error) {
        const errStr = typeof output.error === "string" ? output.error : "";
        let subtext = "Failed";
        if (name === "Bash") {
          for (const line of errStr.split("\n")) {
            const l = line.trim();
            if (l && !l.startsWith("Exit code")) { subtext = l.slice(0, 21); break; }
          }
        } else if (["Edit", "Write", "Read"].includes(name)) {
          const p = (input.filePath || "").split("/").pop();
          if (p) subtext = p.slice(0, 21);
        } else {
          for (const line of errStr.split("\n")) {
            const l = line.trim();
            if (l) { subtext = l.slice(0, 21); break; }
          }
        }
        try {
          await sendNotify("error", `${name} failed`, subtext);
        } catch {}
        return;
      }

      const detail = toolDetail(name, input);
      const sound = soundForTool(name);
      if (!sound) return;

      try {
        await sendNotify(sound, name, detail, false);
      } catch {}
    },

    "permission.asked": async (input, output) => {
      if (!existsSync(SOCKET)) return;

      const toolNameRaw = input.tool || "Unknown";
      const toolInput = input.toolInput || {};

      let displayName = toolNameRaw;
      if (toolNameRaw.includes("__")) {
        const parts = toolNameRaw.split("__");
        if (parts.length >= 2) displayName = `${parts[0]}_${parts[1]}`;
      }

      let detail = "";
      if (toolNameRaw.includes("__")) {
        const parts = toolNameRaw.split("__");
        if (parts.length >= 3) detail = parts[parts.length - 1].slice(0, 21);
      } else if (toolNameRaw === "Bash") {
        detail = (toolInput.description || toolInput.command || "").slice(0, 21);
      } else if (["Edit", "Write", "Read"].includes(toolNameRaw)) {
        const p = (toolInput.filePath || "").split("/").pop();
        if (p) detail = p.slice(0, 21);
      } else if (["WebFetch", "WebSearch"].includes(toolNameRaw)) {
        const v = toolInput.url || toolInput.query || "";
        detail = v.replace(/^https?:\/\//, "").slice(0, 21);
      }

      let result;
      try {
        result = await send({ action: "permission_request", tool: displayName, detail });
      } catch {
        return;
      }

      if (!result) return;

      const status = result.status;

      if (status === "ask") {
        output.decision = { behavior: "ask" };
        return;
      }

      if (status !== "ok") return;

      const allowed = result.allowed === true;
      if (allowed) {
        const decision = { behavior: "allow" };
        if (result.always === true && input.permissionSuggestions) {
          decision.updatedPermissions = input.permissionSuggestions;
        }
        output.decision = decision;
      } else {
        output.decision = { behavior: "deny", message: "Denied on Flipper" };
      }
    },

    "session.idle": async (input) => {
      if (!existsSync(SOCKET)) return;
      try {
        await sendNotify("alert", "OpenCode", "Waiting for input");
      } catch {}
    },

    "session.error": async (input) => {
      if (!existsSync(SOCKET)) return;
      const errMsg = (input.message || input.error || "Error").slice(0, 21);
      try {
        await sendNotify("error", "OpenCode error", errMsg);
      } catch {}
    },
  };
};
