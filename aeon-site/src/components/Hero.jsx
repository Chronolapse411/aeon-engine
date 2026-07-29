import React from 'react';
import TerminalDemo from './TerminalDemo';
import BrowserMockupDemo from './BrowserMockupDemo';

export default function Hero() {
  return (
    <section className="hero">
      {/* Premium background effects */}
      <div className="hero-bg"></div>
      <div className="hero-grid"></div>
      <div className="hero-orb hero-orb--1" aria-hidden="true"></div>
      <div className="hero-orb hero-orb--2" aria-hidden="true"></div>

      <div className="hero-content">
        {/* Status badge */}
        <div className="hero-badge" style={{ animationDelay: '0.2s' }}>
          <span className="pulse"></span>
          Network online · 0 central servers
        </div>

        {/* Main headline */}
        <h1 className="hero-h1-anim">
          The browser<br />
          <span className="gradient">no one controls.</span>
        </h1>

        {/* Tagline */}
        <p className="hero-sub">
          Aeon combines Brave-grade privacy shields and Chrome's sleek ergonomics with
          an autonomous local AI agent pipe — 100% zero telemetry, zero fingerprinting,
          and zero central servers.
        </p>

        {/* CTA buttons */}
        <div className="hero-actions">
          <a className="btn-primary" href="#waitlist" id="hero-download-btn">
            Download Aeon Pro
          </a>
          <a className="btn-secondary" href="#how-it-works">
            Explore Architecture →
          </a>
        </div>

        {/* Sub-note with origin badge */}
        <p className="hero-note">Free Community Edition · $29.99 One-Time Pro · Open source C++ core</p>

        {/* Feature pills */}
        <div className="hero-pills">
          {['Brave Shield Guard', 'Chrome Ergonomics', 'Local MCP AI', 'P2P Updates', 'Zero Telemetry', 'Ed25519 Signed'].map((pill, i) => (
            <span className="hero-pill" key={i} style={{ animationDelay: `${0.6 + i * 0.08}s` }}>
              {pill}
            </span>
          ))}
        </div>

        {/* Brave + Chrome Live Interactive Browser Mockup */}
        <BrowserMockupDemo />

        {/* Live terminal demo */}
        <TerminalDemo />
      </div>
    </section>
  );
}
