// Rift for VS Code: build-and-debug on top of CodeLLDB.
//
// A `rift` launch configuration is resolved here: the program is built with riftc (debug mode:
// full debug info), riftc's diagnostics go to the Problems panel, and the session is handed to
// CodeLLDB (`type: lldb`) with Rift's data formatters loaded and stepping into `std::` allowed.

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
    output = vscode.window.createOutputChannel("Rift");
    diagnostics = vscode.languages.createDiagnosticCollection("riftc");
    context.subscriptions.push(output, diagnostics);

    const provider = new RiftDebugConfigurationProvider();
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider("rift", provider),
        vscode.debug.registerDebugConfigurationProvider("rift", provider, vscode.DebugConfigurationProviderTriggerKind.Dynamic),
        vscode.commands.registerCommand("rift.debugFile", (uri) => debugFile(uri, false)),
        vscode.commands.registerCommand("rift.runFile", (uri) => debugFile(uri, true)),
        vscode.commands.registerCommand("rift.buildFile", async (uri) => {
            const file = fileOf(uri);
            if (file) await buildProgram({ program: file }, folderOf(file));
        }),
    );
    new InsightsController(context, { compilerPath, output });
}

function deactivate() {}

// --- commands ----------------------------------------------------------------------------------

function fileOf(uri) {
    if (uri instanceof vscode.Uri) return uri.fsPath;
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.languageId === "rift") return editor.document.uri.fsPath;
    vscode.window.showErrorMessage("Open a .rifts file first.");
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
        type: "rift",
        request: "launch",
        name: `${noDebug ? "Run" : "Debug"} ${path.basename(file)}`,
        program: file,
    }, { noDebug });
}

// --- debug configurations ----------------------------------------------------------------------

class RiftDebugConfigurationProvider {
    provideDebugConfigurations(folder, _token) {
        const configs = [{
            type: "rift",
            request: "launch",
            name: "Debug current Rift file",
            program: "${file}",
            args: [],
            cwd: "${fileDirname}",
        }];
        const root = folder ? folder.uri.fsPath : undefined;
        if (root && fs.existsSync(path.join(root, "build.rifts"))) {
            for (const target of listTargets(root)) {
                configs.push({ type: "rift", request: "launch", name: `Debug ${target}`, target, args: [], cwd: "${workspaceFolder}" });
            }
        }
        return configs;
    }

    resolveDebugConfiguration(folder, config, _token) {
        // F5 without a launch.json: debug the open file.
        if (!config.type && !config.request && !config.name) {
            const editor = vscode.window.activeTextEditor;
            if (!editor || editor.document.languageId !== "rift") return undefined;
            config.type = "rift";
            config.request = "launch";
            config.name = "Debug current Rift file";
            config.program = "${file}";
            config.cwd = "${fileDirname}";
        }
        if (!config.program && !config.target) {
            vscode.window.showErrorMessage("A Rift launch configuration needs 'program' (a .rifts file or an executable) or 'target' (a target of build.rifts).");
            return undefined;
        }
        return config;
    }

    async resolveDebugConfigurationWithSubstitutedVariables(folder, config, _token) {
        if (!vscode.extensions.getExtension(CODELLDB)) {
            const choice = await vscode.window.showErrorMessage(
                "Debugging Rift needs the CodeLLDB extension (vadimcn.vscode-lldb).", "Install CodeLLDB");
            if (choice) await vscode.commands.executeCommand("workbench.extensions.installExtension", CODELLDB);
            return undefined;
        }
        for (const doc of vscode.workspace.textDocuments) {
            if (doc.languageId === "rift" && doc.isDirty) await doc.save();
        }

        let program = config.program;
        const needsBuild = config.target || (program && /\.rifts$/.test(program));
        if (needsBuild && config.build !== false) {
            program = await buildProgram(config, folder);
            if (!program) return undefined;   // build failed: errors are in Problems
        } else if (needsBuild) {
            program = outputPath(config, folder);
        }

        const settings = vscode.workspace.getConfiguration("rift", folder);
        const initCommands = [`command script import "${path.join(extensionPath, "lldb", "rift_formatters.py")}"`];
        if (settings.get("debug.stepIntoStandardLibrary", true)) {
            // lldb skips functions named `std::...` (meant for C++'s library). Rift's is Rift code.
            initCommands.push("settings set target.process.thread.step-avoid-regexp ''");
        }
        const sourceMap = Object.assign({}, config.sourceMap || {});
        const std = stdLibraryPath(folder);
        if (std) sourceMap["/<std>"] = std;   // riftc writes this when its library/std is gone

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

/** riftc: the setting, else a riftc built in this workspace (the Rift repository), else PATH. */
function compilerPath(folder) {
    const configured = vscode.workspace.getConfiguration("rift", folder).get("compilerPath");
    if (configured) return configured;
    for (const f of vscode.workspace.workspaceFolders || []) {
        for (const candidate of ["build/compiler/riftc", "build/compiler/riftc.exe"]) {
            const p = path.join(f.uri.fsPath, candidate);
            if (fs.existsSync(p)) return p;
        }
    }
    return "riftc";
}

/** The standard library's source: the setting, else library/std in a workspace folder. */
function stdLibraryPath(folder) {
    const configured = vscode.workspace.getConfiguration("rift", folder).get("standardLibraryPath");
    if (configured) return configured;
    for (const f of vscode.workspace.workspaceFolders || []) {
        const p = path.join(f.uri.fsPath, "library", "std");
        if (fs.existsSync(path.join(p, "std.rifts"))) return p;
    }
    return undefined;
}

function buildMode(config, folder) {
    return config.mode || vscode.workspace.getConfiguration("rift", folder).get("debug.buildMode", "debug");
}

/** Where riftc puts the executable (see `riftc --help`). */
function outputPath(config, folder) {
    const mode = buildMode(config, folder);
    if (config.target) {
        const root = projectRoot(config, folder);
        return path.join(root, "build", mode, config.target + (process.platform === "win32" ? ".exe" : ""));
    }
    const file = config.program;
    const stem = path.basename(file, ".rifts");
    return path.join(path.dirname(file), "build", mode, stem + (process.platform === "win32" ? ".exe" : ""));
}

function projectRoot(config, folder) {
    return config.root || (folder ? folder.uri.fsPath : process.cwd());
}

/** Builds the program of `config`; returns the executable's path, or undefined after errors. */
async function buildProgram(config, folder) {
    const riftc = compilerPath(folder);
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

    output.appendLine(`> ${riftc} ${args.join(" ")}   (in ${cwd})`);
    const result = await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Window, title: `riftc: building ${config.target || path.basename(config.program)}` },
        () => run(riftc, args, cwd));
    output.append(result.stdout);
    output.append(result.stderr);
    publishDiagnostics(result.stderr, cwd, folder);

    if (result.error) {
        vscode.window.showErrorMessage(`Cannot run riftc (${riftc}): ${result.error.message}. Set 'rift.compilerPath'.`);
        return undefined;
    }
    if (result.code !== 0) {
        output.show(true);
        vscode.window.showErrorMessage(`riftc: the build failed (exit ${result.code}). See Problems.`);
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

/** `file:line:col: error[E1010]: message` (riftc --diagnostic-format=short) to Problems. */
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
        d.source = "riftc";
        if (m[5]) d.code = m[5];
        if (!byFile.has(file)) byFile.set(file, []);
        byFile.get(file).push(d);
        last = d;
    }
    for (const [file, list] of byFile) diagnostics.set(vscode.Uri.file(file), list);
}

/** Targets of build.rifts, from `riftc build --list` (synchronous: only for the launch.json generator). */
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
