// Edax for VS Code: build-and-debug on top of CodeLLDB.
//
// A `edax` launch configuration is resolved here: the program is built with edaxc (debug mode:
// full debug info), edaxc's diagnostics go to the Problems panel, and the session is handed to
// CodeLLDB (`type: lldb`) with Edax's data formatters loaded and stepping into `std::` allowed.

"use strict";

const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const path = require("path");

const { InsightsController } = require("./insights");

const CODELLDB = "vadimcn.vscode-lldb";

/** @type {vscode.OutputChannel} */
let output;
/** @type {vscode.DiagnosticCollection} */
let diagnostics;
/** @type {string} */
let extensionPath;

function activate(context) {
    extensionPath = context.extensionPath;
    output = vscode.window.createOutputChannel("Edax");
    diagnostics = vscode.languages.createDiagnosticCollection("edaxc");
    context.subscriptions.push(output, diagnostics);

    const provider = new EdaxDebugConfigurationProvider();
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider("edax", provider),
        vscode.debug.registerDebugConfigurationProvider("edax", provider, vscode.DebugConfigurationProviderTriggerKind.Dynamic),
        vscode.commands.registerCommand("edax.debugFile", (uri) => debugFile(uri, false)),
        vscode.commands.registerCommand("edax.runFile", (uri) => debugFile(uri, true)),
        vscode.commands.registerCommand("edax.buildFile", async (uri) => {
            const file = fileOf(uri);
            if (file) await buildProgram({ program: file }, folderOf(file));
        }),
    );
    context.subscriptions.push(vscode.languages.registerDefinitionProvider({ language: "edax" }, new EdaxDefinitionProvider()));
    new InsightsController(context, { compilerPath, output });
}

// --- go to definition ---------------------------------------------------------------------------

/**
 * Ctrl+click / F12: `edaxc definition <file> <line>:<column>` prints where the name at that place is
 * declared. The project root is the nearest folder above the file with a build.ed (else the
 * file's folder), so imports resolve as they do when the program is built. edaxc reads the file
 * from disk: in a file with unsaved changes the places are those of the saved text.
 */
class EdaxDefinitionProvider {
    async provideDefinition(document, position) {
        const file = document.uri.fsPath;
        const folder = folderOf(file);
        const args = ["definition", `--root=${projectRootOf(file)}`, file, `${position.line + 1}:${position.character + 1}`];
        const result = await run(compilerPath(folder), args, path.dirname(file));
        if (result.code !== 0) return undefined;
        const match = /^(.*):(\d+):(\d+)\s*$/.exec(result.stdout.trim());
        if (!match) return undefined;
        const target = path.resolve(path.dirname(file), match[1]);   // edaxc ran in the file's folder
        return new vscode.Location(vscode.Uri.file(target), new vscode.Position(Number(match[2]) - 1, Number(match[3]) - 1));
    }
}

/** The nearest folder above `file` that has a build.ed, else the file's own folder. */
function projectRootOf(file) {
    let dir = path.dirname(file);
    for (;;) {
        if (fs.existsSync(path.join(dir, "build.ed"))) return dir;
        const parent = path.dirname(dir);
        if (parent === dir) return path.dirname(file);
        dir = parent;
    }
}

function deactivate() {}

// --- commands ----------------------------------------------------------------------------------

function fileOf(uri) {
    if (uri instanceof vscode.Uri) return uri.fsPath;
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.languageId === "edax") return editor.document.uri.fsPath;
    vscode.window.showErrorMessage("Open a .ed file first.");
    return undefined;
}

function folderOf(file) {
    return vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file));
}

async function debugFile(uri, noDebug) {
    const file = fileOf(uri);
    if (!file) return;
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.isDirty) await editor.document.save();
    await vscode.debug.startDebugging(folderOf(file), {
        type: "edax",
        request: "launch",
        name: `${noDebug ? "Run" : "Debug"} ${path.basename(file)}`,
        program: file,
    }, { noDebug });
}

// --- debug configurations ----------------------------------------------------------------------

class EdaxDebugConfigurationProvider {
    provideDebugConfigurations(folder, _token) {
        const configs = [{
            type: "edax",
            request: "launch",
            name: "Debug current Edax file",
            program: "${file}",
            args: [],
            cwd: "${fileDirname}",
        }];
        const root = folder ? folder.uri.fsPath : undefined;
        if (root && fs.existsSync(path.join(root, "build.ed"))) {
            for (const target of listTargets(root)) {
                configs.push({ type: "edax", request: "launch", name: `Debug ${target}`, target, args: [], cwd: "${workspaceFolder}" });
            }
        }
        return configs;
    }

    resolveDebugConfiguration(folder, config, _token) {
        // F5 without a launch.json: debug the open file.
        if (!config.type && !config.request && !config.name) {
            const editor = vscode.window.activeTextEditor;
            if (!editor || editor.document.languageId !== "edax") return undefined;
            config.type = "edax";
            config.request = "launch";
            config.name = "Debug current Edax file";
            config.program = "${file}";
            config.cwd = "${fileDirname}";
        }
        if (!config.program && !config.target) {
            vscode.window.showErrorMessage("A Edax launch configuration needs 'program' (a .ed file or an executable) or 'target' (a target of build.ed).");
            return undefined;
        }
        return config;
    }

    async resolveDebugConfigurationWithSubstitutedVariables(folder, config, _token) {
        if (!vscode.extensions.getExtension(CODELLDB)) {
            const choice = await vscode.window.showErrorMessage(
                "Debugging Edax needs the CodeLLDB extension (vadimcn.vscode-lldb).", "Install CodeLLDB");
            if (choice) await vscode.commands.executeCommand("workbench.extensions.installExtension", CODELLDB);
            return undefined;
        }
        for (const doc of vscode.workspace.textDocuments) {
            if (doc.languageId === "edax" && doc.isDirty) await doc.save();
        }

        let program = config.program;
        const needsBuild = config.target || (program && /\.ed$/.test(program));
        if (needsBuild && config.build !== false) {
            program = await buildProgram(config, folder);
            if (!program) return undefined;   // build failed: errors are in Problems
        } else if (needsBuild) {
            program = outputPath(config, folder);
        }

        const settings = vscode.workspace.getConfiguration("edax", folder);
        const initCommands = [`command script import "${path.join(extensionPath, "lldb", "edax_formatters.py")}"`];
        if (settings.get("debug.stepIntoStandardLibrary", true)) {
            // lldb skips functions named `std::...` (meant for C++'s library). Edax's is Edax code.
            initCommands.push("settings set target.process.thread.step-avoid-regexp ''");
        }
        const sourceMap = Object.assign({}, config.sourceMap || {});
        const std = stdLibraryPath(folder);
        if (std) sourceMap["/<std>"] = std;   // edaxc writes this when its library/std is gone

        const lldbConfig = {
            type: "lldb",
            request: "launch",
            name: config.name,
            program,
            args: config.args || [],
            cwd: config.cwd || (folder ? folder.uri.fsPath : path.dirname(program)),
            env: config.env || {},
            stopOnEntry: !!config.stopOnEntry,
            sourceLanguages: ["cpp"],
            sourceMap,
            initCommands: initCommands.concat(config.initCommands || []),
            preRunCommands: config.preRunCommands,
            terminal: config.terminal || "integrated",
            noDebug: config.noDebug,
        };
        for (const key of Object.keys(lldbConfig)) {
            if (lldbConfig[key] === undefined) delete lldbConfig[key];
        }
        return lldbConfig;
    }
}

// --- building ----------------------------------------------------------------------------------

/** edaxc: the setting, else a edaxc built in this workspace (the Edax repository), else PATH. */
function compilerPath(folder) {
    const configured = vscode.workspace.getConfiguration("edax", folder).get("compilerPath");
    if (configured) return configured;
    for (const f of vscode.workspace.workspaceFolders || []) {
        for (const candidate of ["build/compiler/edaxc", "build/compiler/edaxc.exe"]) {
            const p = path.join(f.uri.fsPath, candidate);
            if (fs.existsSync(p)) return p;
        }
    }
    return "edaxc";
}

/** The standard library's source: the setting, else library/std in a workspace folder. */
function stdLibraryPath(folder) {
    const configured = vscode.workspace.getConfiguration("edax", folder).get("standardLibraryPath");
    if (configured) return configured;
    for (const f of vscode.workspace.workspaceFolders || []) {
        const p = path.join(f.uri.fsPath, "library", "std");
        if (fs.existsSync(path.join(p, "std.ed"))) return p;
    }
    return undefined;
}

function buildMode(config, folder) {
    return config.mode || vscode.workspace.getConfiguration("edax", folder).get("debug.buildMode", "debug");
}

/** Where edaxc puts the executable (see `edaxc --help`). */
function outputPath(config, folder) {
    const mode = buildMode(config, folder);
    if (config.target) {
        const root = projectRoot(config, folder);
        return path.join(root, "build", mode, config.target + (process.platform === "win32" ? ".exe" : ""));
    }
    const file = config.program;
    const stem = path.basename(file, ".ed");
    return path.join(path.dirname(file), "build", mode, stem + (process.platform === "win32" ? ".exe" : ""));
}

function projectRoot(config, folder) {
    return config.root || (folder ? folder.uri.fsPath : process.cwd());
}

/** Builds the program of `config`; returns the executable's path, or undefined after errors. */
async function buildProgram(config, folder) {
    const edaxc = compilerPath(folder);
    const mode = buildMode(config, folder);
    const exe = outputPath(config, folder);
    let args;
    let cwd;
    if (config.target) {
        cwd = projectRoot(config, folder);
        args = ["build", config.target, `--mode=${mode}`];
    } else {
        cwd = path.dirname(config.program);
        args = ["build", `--mode=${mode}`, "-o", exe, config.program];
    }
    args.push("--diagnostic-format=short", "--color=never", ...(config.buildArgs || []));

    output.appendLine(`> ${edaxc} ${args.join(" ")}   (in ${cwd})`);
    const result = await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Window, title: `edaxc: building ${config.target || path.basename(config.program)}` },
        () => run(edaxc, args, cwd));
    output.append(result.stdout);
    output.append(result.stderr);
    publishDiagnostics(result.stderr, cwd, folder);

    if (result.error) {
        vscode.window.showErrorMessage(`Cannot run edaxc (${edaxc}): ${result.error.message}. Set 'edax.compilerPath'.`);
        return undefined;
    }
    if (result.code !== 0) {
        output.show(true);
        vscode.window.showErrorMessage(`edaxc: the build failed (exit ${result.code}). See Problems.`);
        vscode.commands.executeCommand("workbench.actions.view.problems");
        return undefined;
    }
    return exe;
}

function run(command, args, cwd) {
    return new Promise((resolve) => {
        cp.execFile(command, args, { cwd, maxBuffer: 64 * 1024 * 1024 }, (error, stdout, stderr) => {
            const code = error && typeof error.code === "number" ? error.code : error ? -1 : 0;
            resolve({ code, stdout: stdout || "", stderr: stderr || "", error: error && error.code === "ENOENT" ? error : undefined });
        });
    });
}

/** `file:line:col: error[E1010]: message` (edaxc --diagnostic-format=short) to Problems. */
function publishDiagnostics(text, cwd, folder) {
    diagnostics.clear();
    const byFile = new Map();
    const re = /^(.+?):(\d+):(\d+): (error|warning|note)(?:\[([A-Z0-9a-z_:]+)\])?: (.*)$/;
    const std = stdLibraryPath(folder);
    let last;
    for (const line of text.split(/\r?\n/)) {
        const m = re.exec(line);
        if (!m) continue;
        let file = m[1];
        if (file.startsWith("<std>/")) {
            if (!std) continue;
            file = path.join(std, file.slice(6));
        } else if (!path.isAbsolute(file)) {
            file = path.join(cwd, file);
        }
        const pos = new vscode.Position(Number(m[2]) - 1, Number(m[3]) - 1);
        if (m[4] === "note" && last) {   // a note belongs to the diagnostic before it
            last.message += `\nnote: ${m[6]}`;
            continue;
        }
        const severity = m[4] === "error" ? vscode.DiagnosticSeverity.Error
            : m[4] === "warning" ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Information;
        const d = new vscode.Diagnostic(new vscode.Range(pos, pos.translate(0, 1)), m[6], severity);
        d.source = "edaxc";
        if (m[5]) d.code = m[5];
        if (!byFile.has(file)) byFile.set(file, []);
        byFile.get(file).push(d);
        last = d;
    }
    for (const [file, list] of byFile) diagnostics.set(vscode.Uri.file(file), list);
}

/** Targets of build.ed, from `edaxc build --list` (synchronous: only for the launch.json generator). */
function listTargets(root) {
    try {
        const text = cp.execFileSync(compilerPath(undefined), ["build", "--list", "--color=never"], { cwd: root, encoding: "utf8" });
        const out = [];
        let inTargets = false;
        for (const line of text.split(/\r?\n/)) {
            if (/^targets:/.test(line)) { inTargets = true; continue; }
            if (/^\S/.test(line)) inTargets = false;
            const m = inTargets && /^\s+(\S+)\s+program\b/.exec(line);
            if (m) out.push(m[1]);
        }
        return out;
    } catch {
        return [];
    }
}

module.exports = { activate, deactivate };
