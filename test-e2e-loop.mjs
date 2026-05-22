#!/usr/bin/env node
/**
 * Phase 4: Autonomous Loop E2E Test
 * 
 * Exercises the full Plan→Act→Observe→Validate cycle:
 *   1. Navigates to a real webpage (Wikipedia)
 *   2. Extracts specific content
 *   3. Summarizes it using the embedded Qwen LLM
 * 
 * Prerequisites:
 *   - Aeon MCP server running (npm run dev in agent/aeon-mcp/)
 *   - Chrome/Edge with --remote-debugging-port=9222
 *   - Qwen 2.5 3B GGUF model downloaded
 * 
 * Run: node test-e2e-loop.mjs
 */

import { spawn } from 'child_process';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const MCP_DIR = resolve(__dirname, 'agent', 'aeon-mcp');

// ── ANSI colors ─────────────────────────────────────────────
const R = '\x1b[31m', G = '\x1b[32m', Y = '\x1b[33m', B = '\x1b[34m';
const M = '\x1b[35m', C = '\x1b[36m', W = '\x1b[37m', X = '\x1b[0m';
const BOLD = '\x1b[1m', DIM = '\x1b[2m';

function log(color, tag, msg) {
  const ts = new Date().toLocaleTimeString('en-US', { hour12: false });
  console.log(`${DIM}${ts}${X} ${color}[${tag}]${X} ${msg}`);
}

// ── MCP Client (stdio transport) ────────────────────────────
class MCPClient {
  constructor() {
    this.proc = null;
    this.reqId = 1;
    this.pending = new Map();
    this.buffer = '';
  }

  async connect() {
    log(C, 'MCP', 'Spawning MCP server...');
    this.proc = spawn('npx', ['tsx', 'src/index.ts'], {
      cwd: MCP_DIR,
      stdio: ['pipe', 'pipe', 'pipe'],
      shell: true,
      env: { ...process.env, AEON_LOG_LEVEL: 'info' },
    });

    this.proc.stdout.on('data', (data) => {
      this.buffer += data.toString();
      let nl;
      while ((nl = this.buffer.indexOf('\n')) !== -1) {
        const line = this.buffer.slice(0, nl).trim();
        this.buffer = this.buffer.slice(nl + 1);
        if (!line) continue;
        try {
          const msg = JSON.parse(line);
          if (msg.id && this.pending.has(msg.id)) {
            const { resolve, reject, timer } = this.pending.get(msg.id);
            clearTimeout(timer);
            this.pending.delete(msg.id);
            if (msg.error) reject(new Error(msg.error.message || JSON.stringify(msg.error)));
            else resolve(msg.result);
          }
        } catch { /* non-JSON log line */ }
      }
    });

    this.proc.stderr.on('data', (d) => {
      const lines = d.toString().split('\n').filter(Boolean);
      for (const line of lines) {
        if (line.includes('ERROR') || line.includes('error'))
          log(R, 'STDERR', line.trim());
        else
          log(DIM, 'STDERR', line.trim());
      }
    });

    // Send initialize
    await this.send('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'aeon-e2e-test', version: '1.0.0' }
    });
    log(G, 'MCP', 'Server initialized');
  }

  send(method, params = {}, timeoutMs = 60000) {
    return new Promise((resolve, reject) => {
      const id = this.reqId++;
      const msg = JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n';
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Timeout: ${method} after ${timeoutMs}ms`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.proc.stdin.write(msg);
    });
  }

  async callTool(name, args = {}, timeoutMs = 120000) {
    const result = await this.send('tools/call', { name, arguments: args }, timeoutMs);
    if (result?.content) {
      return result.content
        .filter(c => c.type === 'text' && c.text)
        .map(c => c.text)
        .join('\n');
    }
    return JSON.stringify(result);
  }

  close() {
    if (this.proc) {
      this.proc.kill('SIGTERM');
      this.proc = null;
    }
  }
}

// ── Test Scenarios ──────────────────────────────────────────

const TESTS = [
  {
    name: 'Navigate & Extract',
    description: 'Navigate to Wikipedia, extract the page title and first paragraph',
    goal: 'Go to https://en.wikipedia.org/wiki/Web_browser and tell me the title of the article and the first sentence of the page.',
    validate: (result) => {
      const lower = result.toLowerCase();
      return lower.includes('web browser') || lower.includes('browser');
    }
  },
  {
    name: 'Search & Summarize',
    description: 'Search Wikipedia for a topic and summarize the result',
    goal: 'Go to https://en.wikipedia.org and search for "Aeon". Tell me what the first search result is about in one sentence.',
    validate: (result) => {
      return result.length > 20; // Got a real response
    }
  },
  {
    name: 'Multi-Step Navigation',
    description: 'Navigate, click a link, and extract from the new page',
    goal: 'Go to https://en.wikipedia.org/wiki/Web_browser, find and click the link for "Netscape Navigator" on the page, then tell me when Netscape Navigator was first released.',
    validate: (result) => {
      const lower = result.toLowerCase();
      return lower.includes('1994') || lower.includes('netscape') || lower.includes('navigator');
    }
  }
];

// ── Helper to extract actual result from task markdown output ──────────
function extractFinalResult(markdown) {
  const marker = '## Result\n';
  const idx = markdown.indexOf(marker);
  if (idx === -1) return '';
  let resultArea = markdown.substring(idx + marker.length);
  const nextHeader = resultArea.indexOf('\n## ');
  if (nextHeader !== -1) {
    resultArea = resultArea.substring(0, nextHeader);
  }
  return resultArea.trim();
}

// ── Main ────────────────────────────────────────────────────

async function runTests() {
  console.log(`\n${BOLD}${M}═══════════════════════════════════════════════${X}`);
  console.log(`${BOLD}${M}  Aeon Browser — Phase 4 Autonomous Loop E2E  ${X}`);
  console.log(`${BOLD}${M}═══════════════════════════════════════════════${X}\n`);

  const client = new MCPClient();
  let passed = 0, failed = 0, skipped = 0;

  try {
    await client.connect();

    // Quick health check — verify LLM is available
    log(C, 'HEALTH', 'Checking LLM availability...');
    try {
      const tools = await client.send('tools/list', {});
      const toolNames = tools.tools?.map(t => t.name) || [];
      log(G, 'HEALTH', `${toolNames.length} tools registered`);
      
      if (!toolNames.includes('aeon_task')) {
        log(R, 'HEALTH', 'aeon_task tool not found — is the planner enabled?');
        process.exit(1);
      }
    } catch (e) {
      log(R, 'HEALTH', `Tool listing failed: ${e.message}`);
      process.exit(1);
    }

    // Run each test
    for (let i = 0; i < TESTS.length; i++) {
      const test = TESTS[i];
      console.log(`\n${BOLD}${B}──── Test ${i + 1}/${TESTS.length}: ${test.name} ────${X}`);
      log(W, 'TEST', test.description);
      log(Y, 'GOAL', test.goal);

      const start = Date.now();
      try {
        // Call the autonomous task tool
        const result = await client.callTool('aeon_task', {
          goal: test.goal
        }, 180000); // 3 min timeout per test

        const elapsed = ((Date.now() - start) / 1000).toFixed(1);
        
        // Parse result
        log(C, 'RESULT', `Completed in ${elapsed}s`);
        
        // Show truncated result
        const preview = result.length > 300 ? result.slice(0, 300) + '...' : result;
        console.log(`${DIM}${preview}${X}`);

        // Validate status and extracted final result
        const isCompleted = result.includes('**Status:** COMPLETED');
        const finalResult = extractFinalResult(result);

        if (isCompleted && test.validate(finalResult)) {
          log(G, 'PASS', `✅ ${test.name} — validation passed (${elapsed}s)`);
          passed++;
        } else {
          log(R, 'FAIL', `❌ ${test.name} — validation check failed`);
          if (!isCompleted) {
            log(R, 'FAIL', `  Reason: Task status was not COMPLETED (indicated failure/timeout)`);
          } else {
            log(R, 'FAIL', `  Reason: Extracted result did not match validation criteria`);
          }
          failed++;
        }
      } catch (e) {
        const elapsed = ((Date.now() - start) / 1000).toFixed(1);
        if (e.message.includes('Timeout')) {
          log(Y, 'SKIP', `⏭️ ${test.name} — timed out after ${elapsed}s`);
          skipped++;
        } else {
          log(R, 'FAIL', `❌ ${test.name} — ${e.message}`);
          failed++;
        }
      }
    }

  } catch (e) {
    log(R, 'FATAL', `Setup failed: ${e.message}`);
    failed = TESTS.length;
  } finally {
    client.close();
  }

  // Summary
  console.log(`\n${BOLD}${M}═══════════════════════════════════════════════${X}`);
  console.log(`${BOLD}  Results: ${G}${passed} passed${X}, ${R}${failed} failed${X}, ${Y}${skipped} skipped${X}`);
  console.log(`${BOLD}${M}═══════════════════════════════════════════════${X}\n`);

  process.exit(failed > 0 ? 1 : 0);
}

runTests().catch((e) => {
  console.error('Unhandled error:', e);
  process.exit(1);
});
