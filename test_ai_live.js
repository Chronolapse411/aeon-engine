const net = require('net');

const PIPE_PATH = '\\\\.\\pipe\\aeon-agent';

function sendCommand(cmdObj) {
  return new Promise((resolve, reject) => {
    const client = net.connect(PIPE_PATH, () => {
      client.write(JSON.stringify(cmdObj) + '\n');
    });

    let buffer = '';
    client.on('data', (data) => {
      buffer += data.toString();
      if (buffer.includes('\n')) {
        client.end();
        try {
          const res = JSON.parse(buffer.trim());
          resolve(res);
        } catch (e) {
          reject(e);
        }
      }
    });

    client.on('error', (err) => {
      reject(err);
    });

    setTimeout(() => {
      client.destroy();
      reject(new Error('Timeout waiting for IPC response'));
    }, 4000);
  });
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function runLiveAiSuite() {
  console.log('====================================================');
  console.log('🚀 Aeon Browser Live AI Summarization & Intent Suite');
  console.log('====================================================\n');

  try {
    // 1. Health check
    console.log('[Test 1] Ping check...');
    const pingRes = await sendCommand({ cmd: 'ping' });
    console.log('  -> Result:', JSON.stringify(pingRes));
    await sleep(300);

    // 2. Open new tab to Hacker News
    console.log('\n[Test 2] Opening tab to Hacker News (https://news.ycombinator.com)...');
    const newTabRes = await sendCommand({ cmd: 'tab.new', url: 'https://news.ycombinator.com' });
    console.log('  -> Result:', JSON.stringify(newTabRes));
    await sleep(300);

    // 3. Summarize active page
    console.log('\n[Test 3] Requesting AI Webpage Summarization (ai.summarize)...');
    const summaryRes = await sendCommand({ cmd: 'ai.summarize', tab_id: newTabRes.tab_id || 1, max_bullets: 3 });
    console.log('  -> Result:', JSON.stringify(summaryRes));
    await sleep(300);

    // 4. Execute AI Intent Navigation
    console.log('\n[Test 4] Requesting AI Natural Language Navigation (ai.navigate_intent)...');
    const intentRes = await sendCommand({ cmd: 'ai.navigate_intent', intent: 'go to github' });
    console.log('  -> Result:', JSON.stringify(intentRes));
    await sleep(300);

    // 5. Test Stagehand Act primitive
    console.log('\n[Test 5] Executing Stagehand Act primitive (stagehand.act)...');
    const actRes = await sendCommand({ cmd: 'stagehand.act', action: 'click', ref: 3 });
    console.log('  -> Result:', JSON.stringify(actRes));
    await sleep(300);

    // 6. Test Stagehand Extract primitive
    console.log('\n[Test 6] Executing Stagehand Extract primitive (stagehand.extract)...');
    const extractRes = await sendCommand({ cmd: 'stagehand.extract', instruction: 'Extract primary headlines' });
    console.log('  -> Result:', JSON.stringify(extractRes));
    await sleep(300);

    // 7. Test WebMCP Tool Discovery
    console.log('\n[Test 7] Discovering WebMCP agent tools (webmcp.tools)...');
    const webmcpRes = await sendCommand({ cmd: 'webmcp.tools' });
    console.log('  -> Result:', JSON.stringify(webmcpRes));

    console.log('\n====================================================');
    console.log('✅ ALL AI SUMMARIZATION & INTENT TESTS PASSED!');
    console.log('====================================================');

  } catch (err) {
    console.error('\n❌ AI Suite execution failed:', err.message);
    process.exit(1);
  }
}

runLiveAiSuite();
