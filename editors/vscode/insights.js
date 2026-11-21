// Code insights: shows what the compiler does that the source doesn't spell out (stack frames,
// heap allocations, drops at the end of scopes, moves, copies, temporaries, implicit conversions,
// virtual calls, run-time checks). The facts come from `edaxc insights <file>` (JSON).
//
// Off unless asked for: `edax.insights.enabled`, toggled with "Edax: Toggle Code Insights".
// Each category has its own setting (`edax.insights.show.<category>`).

"use strict";

const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const path = require("path");

// .ed files are "edax", .edh files "edax-header" (a language each, so each gets its own file icon).
const EDAX_LANGUAGES = ["edax", "edax-header"];
const EDAX_SELECTOR = EDAX_LANGUAGES.map((language) => ({ language }));
function isEdax(doc) { return !!doc && EDAX_LANGUAGES.includes(doc.languageId); }

const CATEGORIES = [
    { id: "stack", label: "Stack frames", icon: "layers", description: "Every slot a function reserves on entry, whichever scope declares it" },
    { id: "heap", label: "Heap memory", icon: "database", description: "Calls that may allocate or free heap memory, and through what" },
    { id: "drops", label: "Drops", icon: "trash", description: "Destructors that run at the end of a scope, or before return / break" },
    { id: "moves", label: "Moves", icon: "arrow-right", description: "Ownership leaving a variable (`move x`, returning a local)" },
    { id: "copies", label: "Large copies", icon: "copy", description: "Copy types bigger than 16 bytes used by value" },
    { id: "temporaries", label: "Temporaries", icon: "symbol-variable", description: "Unnamed stack slots the compiler needs" },
    { id: "conversions", label: "Implicit conversions", icon: "arrow-swap", description: "The few conversions Edax does without a cast" },
    { id: "dispatch", label: "Dynamic dispatch", icon: "references", description: "Virtual and interface calls through a table" },
    { id: "checks", label: "Run-time checks", icon: "shield", description: "Overflow, shift and division checks that can panic" },
];

/** Which fact comes first when several are on one line. */
const PRIORITY = ["heap", "drops", "copies", "moves", "dispatch", "checks", "conversions", "temporaries", "stack"];

class InsightsController {
    /**
     * @param {vscode.ExtensionContext} context
     * @param {{ compilerPath: (folder: any) => string, output: vscode.OutputChannel }} env
     */
    constructor(context, env) {
        this.env = env;
        this.cache = new Map();        // fsPath -> { version, items, error }
        this.running = new Map();      // fsPath -> Promise
        this.lensEmitter = new vscode.EventEmitter();
        this.hintEmitter = new vscode.EventEmitter();
        this.rangeTypes = new Map();   // category -> decoration type (highlight)
        for (const c of CATEGORIES) {
            this.rangeTypes.set(c.id, vscode.window.createTextEditorDecorationType({
                backgroundColor: new vscode.ThemeColor(`edax.insights.${c.id}.background`),
                borderColor: new vscode.ThemeColor(`edax.insights.${c.id}.foreground`),
                borderStyle: "none none dotted none",
                borderWidth: "1px",
            }));
        }
        this.status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
        this.status.command = "edax.insights.choose";

        context.subscriptions.push(
            this.status, this.lensEmitter, this.hintEmitter,
            vscode.languages.registerInlayHintsProvider(EDAX_SELECTOR, {
                onDidChangeInlayHints: this.hintEmitter.event,
                provideInlayHints: (doc, range) => this.inlayHints(doc, range),
            }),
            ...this.rangeTypes.values(),
            vscode.languages.registerCodeLensProvider(EDAX_SELECTOR, {
                onDidChangeCodeLenses: this.lensEmitter.event,
                provideCodeLenses: (doc) => this.codeLenses(doc),
            }),
            vscode.commands.registerCommand("edax.insights.toggle", () => this.toggle()),
            vscode.commands.registerCommand("edax.insights.choose", () => this.choose()),
            vscode.commands.registerCommand("edax.insights.refresh", () => this.refreshVisible(true)),
            vscode.commands.registerCommand("edax.insights.explain", (item) => this.explain(item)),
            vscode.workspace.onDidSaveTextDocument((doc) => { if (isEdax(doc)) this.refresh(doc, true); }),
            vscode.workspace.onDidChangeTextDocument((e) => { if (isEdax(e.document) && e.contentChanges.length) this.lensEmitter.fire(); }),
            vscode.window.onDidChangeVisibleTextEditors(() => this.refreshVisible(false)),
            vscode.window.onDidChangeActiveTextEditor(() => this.updateStatus()),
            vscode.workspace.onDidChangeConfiguration((e) => {
                if (e.affectsConfiguration("edax.insights")) {
                    if (e.affectsConfiguration("edax.insights.enabled")) this.cache.clear();
                    this.refreshVisible(false);
                    this.hintEmitter.fire();
                }
            }),
        );
        this.refreshVisible(false);
    }

    settings(doc) { return vscode.workspace.getConfiguration("edax.insights", doc ? doc.uri : undefined); }
    enabled(doc) { return this.settings(doc).get("enabled", false); }
    shown(doc, category) {
        const def = CATEGORIES.find((c) => c.id === category);
        return def ? this.settings(doc).get(`show.${category}`, true) : false;
    }

    async toggle() {
        const now = !this.enabled();
        await this.settings().update("enabled", now, this.target());
        vscode.window.setStatusBarMessage(`Edax code insights ${now ? "on" : "off"}`, 2000);
    }

    target() {
        return vscode.workspace.workspaceFolders ? vscode.ConfigurationTarget.Workspace : vscode.ConfigurationTarget.Global;
    }

    async choose() {
        const s = this.settings();
        const items = CATEGORIES.map((c) => ({
            label: `$(${c.icon}) ${c.label}`, description: c.description, id: c.id, picked: s.get(`show.${c.id}`, true),
        }));
        const picked = await vscode.window.showQuickPick(items, {
            canPickMany: true,
            title: `Edax code insights (${this.enabled() ? "on" : "off"}) — what to show`,
            placeHolder: "Pick the categories; confirming also turns insights on",
        });
        if (!picked) return;
        const ids = new Set(picked.map((p) => p.id));
        for (const c of CATEGORIES) {
            if (s.get(`show.${c.id}`, true) !== ids.has(c.id)) await s.update(`show.${c.id}`, ids.has(c.id), this.target());
        }
        if (!this.enabled() && ids.size) await s.update("enabled", true, this.target());
    }

    updateStatus() {
        const editor = vscode.window.activeTextEditor;
        if (!editor || !isEdax(editor.document)) { this.status.hide(); return; }
        const on = this.enabled(editor.document);
        const entry = this.cache.get(editor.document.uri.fsPath);
        this.status.text = on ? (entry && entry.error ? "$(eye) Insights $(warning)" : "$(eye) Insights") : "$(eye-closed) Insights";
        this.status.tooltip = on
            ? (entry && entry.error ? `Edax code insights: ${entry.error}` : "Edax code insights are on — click to choose what to show")
            : "Edax code insights are off — click to choose and turn on";
        this.status.show();
    }

    refreshVisible(force) {
        for (const editor of vscode.window.visibleTextEditors) {
            if (isEdax(editor.document)) this.refresh(editor.document, force);
        }
        this.updateStatus();
    }

    async refresh(doc, force) {
        const key = doc.uri.fsPath;
        if (!this.enabled(doc) || doc.uri.scheme !== "file") {
            this.render(doc, []);
            this.lensEmitter.fire();
            this.hintEmitter.fire();
            return;
        }
        const cached = this.cache.get(key);
        if (cached && !force) { this.render(doc, cached.items); this.hintEmitter.fire(); return; }
        if (doc.isDirty) return;   // edaxc reads the file: wait for the save
        if (this.running.has(key)) return this.running.get(key);
        const job = this.run(doc).then((entry) => {
            this.cache.set(key, entry);
            this.running.delete(key);
            this.render(doc, entry.items);
            this.lensEmitter.fire();
            this.hintEmitter.fire();
            this.updateStatus();
        });
        this.running.set(key, job);
        return job;
    }

    /** edaxc insights <file>, from the project root (the folder with build.ed) or the file's folder. */
    run(doc) {
        const file = doc.uri.fsPath;
        const folder = vscode.workspace.getWorkspaceFolder(doc.uri);
        const root = projectRoot(file, folder);
        const edaxc = this.env.compilerPath(folder);
        const args = ["insights", "--color=never", "--diagnostic-format=short"];
        if (root) args.push(`--root=${root}`);
        args.push(file);
        const cwd = root || path.dirname(file);
        return new Promise((resolve) => {
            cp.execFile(edaxc, args, { cwd, maxBuffer: 64 * 1024 * 1024 }, (error, stdout, stderr) => {
                if (error && error.code === "ENOENT") {
                    resolve({ items: [], error: `cannot run edaxc (${edaxc}); set 'edax.compilerPath'` });
                    return;
                }
                if (/unknown command 'insights'/.test(stderr || "")) {
                    resolve({ items: [], error: "this edaxc has no 'insights' command: rebuild the compiler" });
                    return;
                }
                if (error) {
                    const first = (stderr || "").split(/\r?\n/).find((l) => /error/.test(l)) || `edaxc exited with ${error.code}`;
                    resolve({ items: [], error: `the file does not compile: ${first}` });
                    return;
                }
                try {
                    const all = JSON.parse(stdout);
                    const same = (p) => path.resolve(cwd, p) === path.resolve(file);
                    resolve({ items: all.filter((i) => same(i.file)), error: undefined });
                } catch (e) {
                    this.env.output.appendLine(`edaxc insights: unreadable output: ${e.message}`);
                    resolve({ items: [], error: "unreadable output from edaxc insights" });
                }
            });
        });
    }

    /** The insights to show for `doc`, after the category settings. */
    visible(doc, items) {
        const drops = this.shown(doc, "drops");
        return items.filter((i) => {
            if (!this.shown(doc, i.category)) return false;
            // With drops shown, "frees the heap memory of x" is already part of "drop x ... · frees heap memory".
            if (drops && i.category === "heap" && i.kind === "free" && i.placement !== "range") return false;
            return true;
        });
    }

    render(doc, items) {
        const shown = this.enabled(doc) ? this.visible(doc, items) : [];
        for (const editor of vscode.window.visibleTextEditors) {
            if (editor.document !== doc) continue;
            for (const c of CATEGORIES) {
                const mine = shown.filter((i) => i.category === c.id);
                const ranges = mine.filter((i) => i.placement === "range").map((i) => ({
                    range: toRange(doc, i),
                    hoverMessage: hover(doc, i),
                }));
                editor.setDecorations(this.rangeTypes.get(c.id), ranges);
            }
        }
    }

    /**
     * End of each line: one short tag per fact, most important first (`alloc  tmp  tmp  conv`).
     * Hovering a tag explains it, with the code it is about underlined.
     */
    inlayHints(doc, range) {
        if (!this.enabled(doc) || doc.isDirty || !this.settings(doc).get("inlineLabels", true)) return [];
        const entry = this.cache.get(doc.uri.fsPath);
        if (!entry) return [];
        const virtualLines = this.settings(doc).get("virtualLines", true);
        const lines = new Map();
        for (const i of this.visible(doc, entry.items)) {
            const inline = i.placement === "range" || i.placement === "after" || (i.placement === "above" && !virtualLines);
            const line = i.line - 1;
            if (!inline || line < range.start.line || line > range.end.line || line >= doc.lineCount) continue;
            if (!lines.has(line)) lines.set(line, []);
            lines.get(line).push(i);
        }
        const hints = [];
        for (const [line, list] of lines) {
            list.sort((x, y) => PRIORITY.indexOf(x.category) - PRIORITY.indexOf(y.category) || x.column - y.column);
            const parts = [];
            for (const i of list) {
                if (parts.length) parts.push(new vscode.InlayHintLabelPart(" "));
                const part = new vscode.InlayHintLabelPart(tagOf(i).short);
                part.tooltip = hover(doc, i);
                parts.push(part);
            }
            const hint = new vscode.InlayHint(doc.lineAt(line).range.end, parts);
            hint.paddingLeft = true;
            hints.push(hint);
        }
        return hints;
    }

    /** "above" insights as lines of their own: a stack frame above its function, drops above `}`. */
    codeLenses(doc) {
        if (!this.enabled(doc) || !this.settings(doc).get("virtualLines", true) || doc.isDirty) return [];
        const entry = this.cache.get(doc.uri.fsPath);
        if (!entry) return [];
        const groups = new Map();   // "line|category" -> insights
        for (const i of this.visible(doc, entry.items)) {
            if (i.placement !== "above") continue;
            const key = `${i.line}|${i.category}`;
            if (!groups.has(key)) groups.set(key, []);
            groups.get(key).push(i);
        }
        const lenses = [];
        for (const [key, list] of groups) {
            const line = Number(key.split("|")[0]) - 1;
            if (line >= doc.lineCount) continue;
            const c = CATEGORIES.find((x) => x.id === list[0].category);
            const range = doc.lineAt(line).range;
            const title = c.id === "drops" ? dropsTitle(list) : c.id === "stack" ? frameTitle(list[0]) : list.map((i) => i.message).join(" · ");
            lenses.push(new vscode.CodeLens(range, {
                title: `$(${c.icon}) ${title}`,
                tooltip: list.map((i) => stripMarkdown(i.detail)).join("\n\n"),
                command: "edax.insights.explain",
                arguments: [list],
            }));
        }
        return lenses;
    }

    explain(list) {
        if (!Array.isArray(list) || !list.length) return;
        const c = CATEGORIES.find((x) => x.id === list[0].category);
        vscode.window.showInformationMessage(`${c ? c.label : "Insight"}: ${list.map((i) => i.message).join(" · ")}`,
            { modal: true, detail: list.map((i) => stripMarkdown(i.detail)).join("\n\n") });
    }
}

// --- helpers -----------------------------------------------------------------------------------

function projectRoot(file, folder) {
    const stop = folder ? folder.uri.fsPath : path.parse(file).root;
    for (let dir = path.dirname(file); ; dir = path.dirname(dir)) {
        if (fs.existsSync(path.join(dir, "build.ed"))) return dir;
        if (dir === stop || dir === path.dirname(dir)) return undefined;
    }
}

function toRange(doc, i) {
    const start = new vscode.Position(i.line - 1, Math.max(0, i.column - 1));
    let end = new vscode.Position(i.endLine - 1, Math.max(0, i.endColumn - 1));
    if (!end.isAfter(start)) end = start.translate(0, 1);
    return doc.validateRange(new vscode.Range(start, end));
}

/** The short tag shown at the end of the line, and what it stands for. */
function tagOf(i) {
    switch (i.category) {
    case "heap":
        if (i.kind === "free") return { short: "free", long: "frees heap memory" };
        return /in a loop/.test(i.message) ? { short: "alloc↻", long: "may allocate heap memory, in a loop" }
                                           : { short: "alloc", long: "may allocate heap memory" };
    case "drops": return { short: "drop", long: "destructor runs here" };
    case "moves": return { short: "move", long: "ownership moves" };
    case "copies": {
        const m = /copies (\d+) B/.exec(i.message);
        return { short: m ? `copy${m[1]}` : "copy", long: "copies a large value" };
    }
    case "temporaries": return { short: "tmp", long: "temporary stack slot" };
    case "conversions": return { short: "conv", long: "implicit conversion" };
    case "dispatch": return i.kind === "interface" ? { short: "dyn", long: "interface call through a table" }
                                                   : { short: "virt", long: "virtual call through the vtable" };
    case "checks": return { short: "chk", long: "run-time check (can panic)" };
    case "stack": return { short: "frame", long: "stack frame" };
    }
    return { short: i.category, long: i.category };
}

/**
 * A hover: what the tag stands for, the fact, the explanation, and the code with the part in
 * question underlined, like a compiler diagnostic:
 *
 *     85 |     shapes.push(shape::dot);
 *        |            ^^^^
 */
function hover(doc, i) {
    const category = CATEGORIES.find((c) => c.id === i.category) || { icon: "info", label: i.category };
    const tag = tagOf(i);
    const md = new vscode.MarkdownString(undefined, true);
    md.appendMarkdown(`$(${category.icon}) **\`${tag.short}\`** — ${tag.long} *(${category.label})*\n\n`);
    md.appendMarkdown(`**${escape(i.message)}**\n\n`);
    const line = i.line - 1;
    if (line < doc.lineCount) {
        let text = doc.lineAt(line).text;
        let start = Math.min(Math.max(0, i.column - 1), text.length);
        let end = i.endLine === i.line ? Math.max(start + 1, i.endColumn - 1) : text.length;
        end = Math.min(Math.max(end, start + 1), Math.max(text.length, start + 1));
        // Without the indentation, and only around the code in question when the line is long.
        const indent = Math.min(text.length - text.trimStart().length, start);
        text = text.slice(indent); start -= indent; end -= indent;
        const width = 72;
        if (text.length > width) {
            const from = Math.max(0, Math.min(start - 24, text.length - width));
            const to = Math.min(text.length, Math.max(from + width, end + 8));
            const head = from > 0 ? "\u2026" : "";
            text = head + text.slice(from, to) + (to < text.length ? "\u2026" : "");
            start = start - from + head.length; end = end - from + head.length;
        }
        const number = String(i.line);
        const gutter = " ".repeat(number.length);
        const marks = i.placement === "range" ? "^".repeat(Math.max(1, end - start)) : "^";
        md.appendCodeblock(`${number} | ${text}\n${gutter} | ${" ".repeat(start)}${marks}`, "text");
    }
    if (i.detail) md.appendMarkdown(`\n${i.detail}`);
    md.isTrusted = false;
    return md;
}

/** "stack frame: 643 B on entry · 9 variables · 22 temporaries" (the slots are in the details). */
function frameTitle(i) {
    const m = /^stack frame of (.+?): (\d+) B reserved on entry(?:, about (\d+) B optimized)?/.exec(i.message);
    if (!m) return i.message;
    const optimized = m[3] !== undefined ? ` (~${m[3]} B optimized)` : "";
    const temps = /(\d+) temporar/.exec(i.message);
    const flags = /(\d+) drop flag/.exec(i.message);
    const named = (i.detail.match(/\| (variable|parameter) \|/g) || []).length;
    return `stack frame: ${m[2]} B on entry${optimized} · ${named} variable${named === 1 ? "" : "s"}` +
        (temps ? ` · ${temps[1]} temporar${temps[1] === "1" ? "y" : "ies"}` : "") + (flags ? ` · ${flags[1]} drop flag${flags[1] === "1" ? "" : "s"}` : "");
}

function escape(text) { return text.replace(/[\\`*_{}[\]<>#|]/g, (c) => "\\" + c); }

function stripMarkdown(text) {
    return (text || "").replace(/`/g, "").replace(/^\|.*\|$/gm, (row) => row.replace(/^\|\s*|\s*\|$/g, "").split(/\s*\|\s*/).join("   ")).replace(/^[-\s]+$/gm, "");
}

/** "end of scope: drop tmp, list (if not moved) — frees heap: tmp, list" */
function dropsTitle(list) {
    const parsed = list.map((i) => /^drop (\S+): (.+?)( \(if not moved\))?( · frees heap memory)?$/.exec(i.message) || [i.message, i.message]);
    const names = parsed.map((m) => m[1] + (m[3] ? " (if not moved)" : ""));
    const freeing = parsed.filter((m) => m[4]).map((m) => m[1]);
    const where = list.some((i) => i.kind === "exit-drop") ? "leaving" : "scope end";
    return `${where}: drop ${names.join(", ")}${freeing.length ? " · frees heap" : ""}`;
}

module.exports = { InsightsController, CATEGORIES, EDAX_SELECTOR, isEdax };
