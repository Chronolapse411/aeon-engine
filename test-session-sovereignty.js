// AeonBrowser — test-session-sovereignty.js
// Automated verification script for Milestone 2 IPC Session Sovereignty

const net = require('net');
const fs = require('fs');
const path = require('path');

const PIPE_NAME = '\\\\.\\pipe\\aeon-agent';
const testAuthPath = path.join(__dirname, 'test_auth.json');
const testExportPath = path.join(__dirname, 'test_auth_exported.json');

// 1. Create a dummy test_auth.json for import verification
const sampleAuth = {
  cookies: [
    {
      name: "aeon_auth_token",
      value: "sovereign_session_token_999",
      domain: ".aeonbrowser.org",
      path: "/",
      expires: 1800000000.0,
      httpOnly: true,
      secure: true,
      sameSite: "Lax"
    }
  ],
  origins: [
    {
      origin: "https://aeonbrowser.org",
      localStorage: [
        {
          name: "user_preference",
          value: "stealth_mode_active"
        }
      ]
    }
  ]
};

fs.writeFileSync(testAuthPath, JSON.stringify(sampleAuth, null, 2));
console.log('[Test] Created test input auth.json:', testAuthPath);

// 2. Connect to named pipe server
const client = net.connect(PIPE_NAME, () => {
  console.log('[Test] Connected to Aeon IPC pipe:', PIPE_NAME);

  // Send session.import command
  const importCmd = JSON.stringify({ cmd: 'session.import', path: testAuthPath }) + '\n';
  console.log('[Test] Sending session.import command...');
  client.write(importCmd);
});

let buffer = '';
client.on('data', (data) => {
  buffer += data.toString();
  if (buffer.includes('\n')) {
    const lines = buffer.split('\n');
    for (const line of lines) {
      if (!line.trim()) continue;
      console.log('[IPC Response]', line);
      try {
        const res = JSON.parse(line);
        if (res.session_imported) {
          console.log('[Test] VERIFIED: session.import returned session_imported: true');
          console.log(`[Test] Imported Cookies: ${res.imported_cookies}, Imported Origins: ${res.imported_origins}`);

          // Now test session.export
          console.log('[Test] Sending session.export command...');
          const exportCmd = JSON.stringify({ cmd: 'session.export', path: testExportPath }) + '\n';
          client.write(exportCmd);
        } else if (res.exported) {
          console.log('[Test] VERIFIED: session.export returned exported: true');
          console.log(`[Test] Exported Cookies: ${res.cookies_count}, Exported Origins: ${res.origins_count}`);
          console.log(`[Test] Exported Path: ${res.storage_state_path}`);
          
          if (fs.existsSync(testExportPath)) {
            console.log('[Test] Exported auth.json file exists on disk!');
            const content = fs.readFileSync(testExportPath, 'utf8');
            console.log('[Test] Content preview:\n' + content.slice(0, 300));
          }
          client.end();
          process.exit(0);
        }
      } catch (err) {
        console.error('[Test] Error parsing IPC response:', err);
      }
    }
  }
});

client.on('error', (err) => {
  console.error('[Test] Pipe connection error:', err.message);
  console.error('[Test] Make sure Aeon.exe is running with IPC pipe active.');
  process.exit(1);
});

client.on('end', () => {
  console.log('[Test] Pipe connection closed.');
});
