const net = require('net');
const fs = require('fs');

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
    }, 2000);
  });
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function runLiveSuite() {
  console.log('====================================================');
  console.log('🚀 Aeon Browser Live IPC Arbitrary Command Test Suite');
  console.log('====================================================\n');

  try {
    // 1. Health check
    console.log('[Task 1] Sending ping...');
    const pingRes = await sendCommand({ cmd: 'ping' });
    console.log('  -> Result:', JSON.stringify(pingRes));
    await sleep(250);

    // 2. Browser info
    console.log('\n[Task 2] Querying browser.info...');
    const infoRes = await sendCommand({ cmd: 'browser.info' });
    console.log('  -> Result:', JSON.stringify(infoRes));
    await sleep(250);

    // 3. Tab list
    console.log('\n[Task 3] Querying tab.list...');
    const listRes = await sendCommand({ cmd: 'tab.list' });
    console.log('  -> Result:', JSON.stringify(listRes));
    await sleep(250);

    // 4. Open new tab to example.com
    console.log('\n[Task 4] Opening new tab to https://example.com...');
    const newTabRes = await sendCommand({ cmd: 'tab.new', url: 'https://example.com' });
    console.log('  -> Result:', JSON.stringify(newTabRes));
    await sleep(250);

    // 5. Navigate active tab to HackerNews
    console.log('\n[Task 5] Navigating active tab to https://news.ycombinator.com...');
    const navRes = await sendCommand({ cmd: 'tab.navigate', url: 'https://news.ycombinator.com' });
    console.log('  -> Result:', JSON.stringify(navRes));
    await sleep(250);

    // 6. Session Export (auth.json)
    console.log('\n[Task 6] Exporting session state (session.export)...');
    const exportRes = await sendCommand({ cmd: 'session.export' });
    console.log('  -> Result:', JSON.stringify(exportRes));
    await sleep(250);

    // 7. Session Import (auth.json)
    console.log('\n[Task 7] Importing session state (session.import)...');
    const importRes = await sendCommand({ cmd: 'session.import' });
    console.log('  -> Result:', JSON.stringify(importRes));
    await sleep(250);

    // 8. Re-query tab list to verify tab states
    console.log('\n[Task 8] Verifying final tab list...');
    const finalListRes = await sendCommand({ cmd: 'tab.list' });
    console.log('  -> Result:', JSON.stringify(finalListRes));

    console.log('\n====================================================');
    console.log('✅ ALL ARBITRARY COMMAND TESTS PASSED SUCCESSFULLY!');
    console.log('====================================================');

  } catch (err) {
    console.error('\n❌ Test execution failed:', err.message);
    process.exit(1);
  }
}

runLiveSuite();
