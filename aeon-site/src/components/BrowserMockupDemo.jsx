import React, { useState } from 'react';

export default function BrowserMockupDemo() {
  const [activeTab, setActiveTab] = useState(0);
  const [shieldActive, setShieldActive] = useState(true);

  const tabs = [
    {
      id: 0,
      title: '⚡ Aeon Sovereign AI',
      url: 'aeon://sovereign-agent',
      shieldCount: '1,428',
      content: {
        heading: 'Autonomous Web Intelligence Agent',
        status: 'AI Agent Active: Executing deep research sweep',
        badge: 'Local MCP Pipeline',
        logs: [
          'Connecting to local AeonWebMCP pipe...',
          'Parsing DOM snapshot with Rust tree serializer...',
          'Bypass tracker injected — 0 telemetry data leaked',
          'Autonomous task complete: 100% verified clean'
        ]
      }
    },
    {
      id: 1,
      title: '🛡️ Brave Shield Guard',
      url: 'aeon://shield-statistics',
      shieldCount: '2,891',
      content: {
        heading: 'Sovereign Privacy Shield Matrix',
        status: 'Brave-Grade Shield: 100% Protection Level',
        badge: 'Rust AdBlock Engine',
        logs: [
          'Trackers blocked today: 2,891',
          'Bandwidth saved: 418 MB',
          'Fingerprinting attempts thwarted: 94',
          'HTTPS-Everywhere enforced: Active'
        ]
      }
    },
    {
      id: 2,
      title: '🌐 Sovereign P2P Network',
      url: 'aeon://p2p-mesh',
      shieldCount: '847',
      content: {
        heading: 'Decentralized P2P Patch Network',
        status: 'Mesh Active: Connected to 847 peer nodes',
        badge: 'Ed25519 Signed',
        logs: [
          'Verified manifest signature: Ed25519 PASS',
          'Delta package received via encrypted webRTC mesh',
          'Zero central server reliance — zero kill switches'
        ]
      }
    }
  ];

  const currentTab = tabs[activeTab];

  return (
    <div className="browser-mockup-container reveal">
      {/* Chrome Window Header */}
      <div className="chrome-header">
        {/* Top Control Dots & Titlebar */}
        <div className="chrome-top-row">
          <div className="window-dots">
            <span className="dot dot-close"></span>
            <span className="dot dot-min"></span>
            <span className="dot dot-max"></span>
          </div>

          {/* Chrome-Style Tab Strip */}
          <div className="tab-strip">
            {tabs.map((t, idx) => (
              <button
                key={t.id}
                className={`tab-item ${activeTab === idx ? 'active' : ''}`}
                onClick={() => setActiveTab(idx)}
              >
                <span className="tab-title">{t.title}</span>
                <span className="tab-close">×</span>
              </button>
            ))}
            <button className="tab-add">+</button>
          </div>
        </div>

        {/* Omnibox & Navigation Row */}
        <div className="chrome-nav-row">
          <div className="nav-buttons">
            <button className="nav-btn" title="Back">←</button>
            <button className="nav-btn" title="Forward">→</button>
            <button className="nav-btn" title="Reload">↻</button>
          </div>

          {/* Floating Pill URL Bar */}
          <div className="url-bar">
            <span className="lock-icon">🔒</span>
            <span className="url-text">{currentTab.url}</span>
            
            {/* Brave-Inspired Privacy Shield Badge */}
            <button
              className={`shield-badge ${shieldActive ? 'active' : ''}`}
              onClick={() => setShieldActive(!shieldActive)}
              title="Toggle Sovereign Shield"
            >
              <span className="shield-icon">🛡️</span>
              <span className="shield-count">{currentTab.shieldCount}</span>
            </button>
          </div>

          {/* Right Action Icons */}
          <div className="action-icons">
            <span className="action-btn" title="Downloads">⬇</span>
            <span className="action-btn" title="Bookmarks">★</span>
            <span className="action-btn" title="Local AI">⚡</span>
            <span className="action-btn" title="Menu">⋮</span>
          </div>
        </div>
      </div>

      {/* Simulated Browser Web View Body */}
      <div className="chrome-body">
        <div className="page-card">
          <div className="page-header">
            <div>
              <span className="status-tag">{currentTab.content.badge}</span>
              <h3 className="page-h3">{currentTab.content.heading}</h3>
            </div>
            <div className="pulse-indicator">
              <span className="pulse-dot"></span>
              {currentTab.content.status}
            </div>
          </div>

          <div className="log-console">
            {currentTab.content.logs.map((log, i) => (
              <div key={i} className="log-line">
                <span className="log-time">08:18:{10 + i * 4}</span>
                <span className="log-msg">{log}</span>
              </div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
