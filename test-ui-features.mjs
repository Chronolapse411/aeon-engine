import { spawn, execSync } from 'child_process';
import net from 'net';
import fs from 'fs';
import path from 'path';

// Locate settings.json path
const appData = process.env.APPDATA || (process.platform === 'win32' ? path.join(process.env.USERPROFILE, 'AppData', 'Roaming') : '');
const settingsPath = path.join(appData, 'DelgadoLogic', 'Aeon', 'settings.json');

console.log('Using settings path:', settingsPath);

// Helper to read tor_enabled from settings.json
function getTorEnabled() {
    try {
        if (fs.existsSync(settingsPath)) {
            const data = fs.readFileSync(settingsPath, 'utf8');
            const match = data.match(/"tor_enabled":\s*(true|false)/);
            if (match) {
                return match[1] === 'true';
            }
        }
    } catch (e) {
        console.error('Error reading settings.json:', e);
    }
    return false;
}

// Start browser process
console.log('Launching Aeon.exe from publish/Pro/ ...');
const aeonProcess = spawn(path.join('publish', 'Pro', 'Aeon.exe'), [], {
    detached: true,
    stdio: 'ignore'
});
aeonProcess.unref();

// Connect to named pipe with retry logic
const PIPE_PATH = '\\\\.\\pipe\\aeon-agent';
let client = null;
let connected = false;

async function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

// Retry loop to connect to Named Pipe
for (let i = 0; i < 30; i++) {
    try {
        console.log(`Connecting to named pipe: ${PIPE_PATH} (attempt ${i + 1}/30)`);
        client = net.connect(PIPE_PATH);
        await new Promise((resolve, reject) => {
            client.once('connect', () => {
                connected = true;
                resolve();
            });
            client.once('error', (err) => {
                reject(err);
            });
        });
        if (connected) {
            console.log('Connected to Named Pipe successfully!');
            break;
        }
    } catch (e) {
        await sleep(1000);
    }
}

if (!connected) {
    console.error('Failed to connect to browser named pipe.');
    process.exit(1);
}

// Send command helper
async function sendCommand(cmdObj) {
    return new Promise((resolve, reject) => {
        const payload = JSON.stringify(cmdObj) + '\n';
        client.write(payload);
        client.once('data', (data) => {
            try {
                const response = JSON.parse(data.toString().trim());
                resolve(response);
            } catch (e) {
                reject(e);
            }
        });
    });
}

// PowerShell Win32 mouse click helper using semicolons and standard string literals
function clickButton(hwnd, btnId) {
    const psCommand = `
$sig = '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam); [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect); public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }'
Add-Type -MemberDefinition $sig -Name "Win32Utils" -Namespace "Win32" -ErrorAction SilentlyContinue
$rect = New-Object Win32.Win32Utils+RECT
[Win32.Win32Utils]::GetClientRect(${hwnd}, [ref]$rect)
$width = $rect.Right
$toolbarStart = $width - 138 - 192 - 8
$btnOffset = 0
switch (${btnId}) {
    20 { $btnOffset = 0 * 32 + 14 }
    21 { $btnOffset = 1 * 32 + 14 }
    22 { $btnOffset = 2 * 32 + 14 }
    23 { $btnOffset = 3 * 32 + 14 }
    24 { $btnOffset = 4 * 32 + 14 }
}
$x = $toolbarStart + $btnOffset
$y = 20
$lParam = [IntPtr](($y -shl 16) -bor $x)
[Win32.Win32Utils]::PostMessage(${hwnd}, 0x0201, 1, $lParam)
Start-Sleep -Milliseconds 50
[Win32.Win32Utils]::PostMessage(${hwnd}, 0x0202, 0, $lParam)
`;
    fs.writeFileSync('click.ps1', psCommand);
    try {
        execSync('powershell -ExecutionPolicy Bypass -File click.ps1');
    } finally {
        if (fs.existsSync('click.ps1')) {
            fs.unlinkSync('click.ps1');
        }
    }
}

// Run test pipeline
try {
    // Ping to verify responsiveness
    const pingRes = await sendCommand({ cmd: 'ping' });
    console.log('Ping Response:', pingRes);

    // Get browser info
    const infoRes = await sendCommand({ cmd: 'browser.info' });
    console.log('Browser Info:', infoRes);
    const hwnd = infoRes.hwnd;

    if (!hwnd) {
        throw new Error('Failed to retrieve HWND from browser info');
    }

    console.log(`Target window HWND: ${hwnd}`);

    // --- TEST 1: Tor Button Toggle ---
    const initialTor = getTorEnabled();
    console.log(`Initial Tor State: ${initialTor}`);
    console.log('Clicking Tor button...');
    clickButton(hwnd, 22);
    await sleep(2000);
    const afterTor = getTorEnabled();
    console.log(`After Tor State: ${afterTor}`);
    if (afterTor === initialTor) {
        throw new Error('Tor state did not toggle correctly in settings.json');
    }
    console.log('✓ Tor button toggle test passed!');

    // Toggle back to clean up
    console.log('Toggling Tor button back...');
    clickButton(hwnd, 22);
    await sleep(1500);

    // --- TEST 2: Downloads Navigation ---
    console.log('Clicking Downloads button...');
    clickButton(hwnd, 20);
    await sleep(2000);
    const tabDownloads = await sendCommand({ cmd: 'tab.active' });
    console.log('Active Tab after Downloads click:', tabDownloads.tab);
    if (tabDownloads.tab.url !== 'aeon://downloads') {
        throw new Error(`Expected active tab URL to be aeon://downloads, got: ${tabDownloads.tab.url}`);
    }
    console.log('✓ Downloads button test passed!');

    // --- TEST 3: Bookmarks Navigation ---
    console.log('Clicking Bookmarks button...');
    clickButton(hwnd, 21);
    await sleep(2000);
    const tabBookmarks = await sendCommand({ cmd: 'tab.active' });
    console.log('Active Tab after Bookmarks click:', tabBookmarks.tab);
    if (tabBookmarks.tab.url !== 'aeon://bookmarks') {
        throw new Error(`Expected active tab URL to be aeon://bookmarks, got: ${tabBookmarks.tab.url}`);
    }
    console.log('✓ Bookmarks button test passed!');

    // --- TEST 4: AI Summary Navigation ---
    console.log('Clicking AI Summary button...');
    clickButton(hwnd, 23);
    await sleep(2000);
    const tabAISummary = await sendCommand({ cmd: 'tab.active' });
    console.log('Active Tab after AI Summary click:', tabAISummary.tab);
    if (tabAISummary.tab.url !== 'aeon://intelligence') {
        throw new Error(`Expected active tab URL to be aeon://intelligence, got: ${tabAISummary.tab.url}`);
    }
    console.log('✓ AI Summary button test passed!');

    // --- TEST 5: AI Journey Navigation ---
    console.log('Clicking AI Journey button...');
    clickButton(hwnd, 24);
    await sleep(2000);
    const tabAIJourney = await sendCommand({ cmd: 'tab.active' });
    console.log('Active Tab after AI Journey click:', tabAIJourney.tab);
    if (tabAIJourney.tab.url !== 'aeon://journey') {
        throw new Error(`Expected active tab URL to be aeon://journey, got: ${tabAIJourney.tab.url}`);
    }
    console.log('✓ AI Journey button test passed!');

    console.log('\n=========================================');
    console.log('ALL E2E UI TESTS PASSED SUCCESSFULLY! 🚀');
    console.log('=========================================');

} catch (e) {
    console.error('Test pipeline failed:', e);
    process.exit(1);
} finally {
    if (client) {
        client.end();
    }
    // Exit browser process
    console.log('Cleaning up browser process...');
    try {
        execSync(`powershell -Command "Stop-Process -Name Aeon -Force"`, { stdio: 'ignore' });
    } catch (err) {}
    process.exit(0);
}
