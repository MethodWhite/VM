(function() {
  'use strict';

  const API_BASE = 'https://api.vexpm.dev/v1';
  const STORAGE_CACHE_KEY = 'vex_registry_cache';
  const CACHE_TTL = 5 * 60 * 1000;

  const RegistryApp = {
    allPackages: [],
    filteredPackages: [],
    currentSort: 'downloads',
    searchQuery: '',
    currentPackage: null,

    async init() {
      this.bindEvents();

      if (this.isPackageDetailPage()) {
        await this.loadPackageDetail();
      } else {
        await this.loadPackageList();
      }
    },

    isPackageDetailPage() {
      return document.getElementById('package-detail') !== null;
    },

    getPackageNameFromUrl() {
      const params = new URLSearchParams(window.location.search);
      return params.get('name');
    },

    async fetchWithCache(url, cacheKey) {
      const cached = localStorage.getItem(cacheKey);
      if (cached) {
        try {
          const parsed = JSON.parse(cached);
          if (Date.now() - parsed.timestamp < CACHE_TTL) {
            return parsed.data;
          }
        } catch {}
      }

      try {
        const resp = await fetch(url);
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const data = await resp.json();
        localStorage.setItem(cacheKey, JSON.stringify({ data, timestamp: Date.now() }));
        return data;
      } catch (err) {
        if (cached) {
          try { return JSON.parse(cached).data; } catch {}
        }
        throw err;
      }
    },

    async loadPackageList() {
      const listEl = document.getElementById('package-list');
      const loadingEl = document.getElementById('loading-indicator');
      const countEl = document.getElementById('package-count');
      const errorEl = document.getElementById('error-indicator');

      try {
        loadingEl.style.display = 'block';
        listEl.innerHTML = '';

        const data = await this.fetchWithCache(`${API_BASE}/packages`, STORAGE_CACHE_KEY);
        this.allPackages = (data.packages || data || []).sort((a, b) => (b.downloads || 0) - (a.downloads || 0));
        this.applyFilters();
        loadingEl.style.display = 'none';
      } catch (err) {
        loadingEl.style.display = 'none';
        errorEl.style.display = 'block';
        errorEl.textContent = `Error al cargar paquetes: ${err.message}. Usando datos de demostración.`;
        this.loadDemoPackages();
      }
    },

    loadDemoPackages() {
      this.allPackages = [
        { name: 'vex-http', version: '0.4.1', description: 'Cliente HTTP asíncrono para Vex', downloads: 15420, updated: '2026-06-10', author: 'vex-lang', license: 'MIT', dependencies: ['vex-core'] },
        { name: 'vex-json', version: '1.2.0', description: 'Parser y serializer JSON', downloads: 12300, updated: '2026-06-08', author: 'vex-lang', license: 'MIT', dependencies: [] },
        { name: 'vex-test', version: '0.8.3', description: 'Framework de testing unitario', downloads: 9870, updated: '2026-06-05', author: 'vex-lang', license: 'Apache-2.0', dependencies: [] },
        { name: 'vex-cli', version: '2.1.0', description: 'Herramientas de línea de comandos para proyectos Vex', downloads: 8750, updated: '2026-06-01', author: 'vex-lang', license: 'MIT', dependencies: ['vex-json'] },
        { name: 'vex-sql', version: '0.3.0', description: 'Driver SQL para Vex', downloads: 6540, updated: '2026-05-28', author: 'community', license: 'MIT', dependencies: ['vex-core'] },
        { name: 'vex-regex', version: '0.5.1', description: 'Expresiones regulares para Vex', downloads: 5430, updated: '2026-05-25', author: 'community', license: 'MIT', dependencies: [] },
        { name: 'vex-crypto', version: '0.2.0', description: 'Funciones criptográficas (SHA256, AES)', downloads: 4320, updated: '2026-05-20', author: 'vex-lang', license: 'MIT', dependencies: ['vex-core'] },
        { name: 'vex-fs', version: '1.0.0', description: 'Operaciones del sistema de archivos', downloads: 3980, updated: '2026-05-15', author: 'vex-lang', license: 'Apache-2.0', dependencies: [] },
        { name: 'vex-log', version: '0.6.2', description: 'Logger estructurado para Vex', downloads: 3210, updated: '2026-05-10', author: 'community', license: 'MIT', dependencies: [] },
        { name: 'vex-collections', version: '0.7.0', description: 'Estructuras de datos adicionales (HashMap, TreeSet)', downloads: 2870, updated: '2026-05-05', author: 'vex-lang', license: 'MIT', dependencies: ['vex-core'] },
        { name: 'vex-time', version: '0.4.0', description: 'Manejo de fechas y tiempos', downloads: 2540, updated: '2026-04-30', author: 'community', license: 'MIT', dependencies: [] },
        { name: 'vex-math', version: '1.1.0', description: 'Funciones matemáticas avanzadas', downloads: 2100, updated: '2026-04-25', author: 'vex-lang', license: 'MIT', dependencies: [] },
      ];
      this.applyFilters();
    },

    applyFilters() {
      let list = [...this.allPackages];

      if (this.searchQuery) {
        const q = this.searchQuery.toLowerCase();
        list = list.filter(p =>
          p.name.toLowerCase().includes(q) ||
          (p.description || '').toLowerCase().includes(q) ||
          (p.author || '').toLowerCase().includes(q)
        );
      }

      switch (this.currentSort) {
        case 'name': list.sort((a, b) => a.name.localeCompare(b.name)); break;
        case 'downloads': list.sort((a, b) => (b.downloads || 0) - (a.downloads || 0)); break;
        case 'updated': list.sort((a, b) => (b.updated || '').localeCompare(a.updated || '')); break;
        case 'version': list.sort((a, b) => (b.version || '').localeCompare(a.version || '')); break;
      }

      this.filteredPackages = list;
      this.renderPackageList();
    },

    renderPackageList() {
      const listEl = document.getElementById('package-list');
      const countEl = document.getElementById('package-count');

      countEl.textContent = `${this.filteredPackages.length} paquete${this.filteredPackages.length !== 1 ? 's' : ''}`;

      if (this.filteredPackages.length === 0) {
        listEl.innerHTML = '<div class="alert alert-info" style="text-align:center;padding:2rem">No se encontraron paquetes.</div>';
        return;
      }

      listEl.innerHTML = this.filteredPackages.map(p => `
        <div class="card package-card" data-name="${p.name}">
          <div class="card-header">
            <div>
              <div class="card-title">
                <a href="?name=${encodeURIComponent(p.name)}" class="package-name">${p.name}</a>
                <span class="badge badge-blue">${p.version}</span>
              </div>
              <div class="card-subtitle">por ${p.author || 'desconocido'}</div>
            </div>
            <div class="package-stats">
              <span class="tooltip" data-tip="Descargas">⬇ ${this.formatNumber(p.downloads || 0)}</span>
            </div>
          </div>
          <div class="card-body">
            <p>${p.description || 'Sin descripción'}</p>
            <div style="margin-top:0.5rem;display:flex;gap:0.5rem;flex-wrap:wrap">
              <span class="badge">${p.license || 'Sin licencia'}</span>
              <span class="badge badge-orange">${p.updated || ''}</span>
            </div>
          </div>
        </div>
      `).join('');

      listEl.querySelectorAll('.package-card').forEach(el => {
        el.addEventListener('click', (e) => {
          if (e.target.tagName !== 'A') {
            const name = el.dataset.name;
            window.location.href = `?name=${encodeURIComponent(name)}`;
          }
        });
      });
    },

    async loadPackageDetail() {
      const name = this.getPackageNameFromUrl();
      if (!name) {
        document.getElementById('package-detail').innerHTML = '<div class="alert alert-error">No se especificó un paquete.</div>';
        return;
      }

      const detailEl = document.getElementById('package-detail');
      const loadingEl = document.getElementById('detail-loading');

      try {
        loadingEl.style.display = 'block';

        let pkg;
        try {
          pkg = await this.fetchWithCache(`${API_BASE}/packages/${encodeURIComponent(name)}`, `${STORAGE_CACHE_KEY}_${name}`);
        } catch {
          pkg = this.allPackages.find(p => p.name === name) || null;
        }

        loadingEl.style.display = 'none';

        if (!pkg) {
          detailEl.innerHTML = '<div class="alert alert-error">Paquete no encontrado.</div>';
          return;
        }

        this.currentPackage = pkg;
        this.renderPackageDetail(pkg);
      } catch (err) {
        loadingEl.style.display = 'none';
        detailEl.innerHTML = `<div class="alert alert-error">Error: ${err.message}</div>`;
      }
    },

    renderPackageDetail(pkg) {
      const el = document.getElementById('package-detail');
      el.innerHTML = `
        <div class="card" style="margin-bottom:1rem">
          <div style="display:flex;justify-content:space-between;align-items:flex-start;flex-wrap:wrap;gap:1rem">
            <div>
              <h1 style="margin:0 0 0.3rem 0">${pkg.name}</h1>
              <div style="display:flex;gap:0.5rem;flex-wrap:wrap;align-items:center">
                <span class="badge badge-blue">${pkg.version}</span>
                <span class="badge">${pkg.license || 'Sin licencia'}</span>
                <span style="font-size:0.85rem;color:var(--text-secondary)">por ${pkg.author || 'desconocido'}</span>
              </div>
            </div>
            <div style="text-align:right">
              <div style="font-size:1.5rem;font-weight:600;color:var(--accent-green)">${this.formatNumber(pkg.downloads || 0)}</div>
              <div style="font-size:0.8rem;color:var(--text-muted)">descargas totales</div>
            </div>
          </div>
        </div>

        <div class="card" style="margin-bottom:1rem">
          <h3>Descripción</h3>
          <p style="color:var(--text-secondary)">${pkg.description || 'Sin descripción.'}</p>
        </div>

        <div style="display:grid;grid-template-columns:1fr 1fr;gap:1rem;margin-bottom:1rem">
          <div class="card">
            <h3>Información</h3>
            <table style="width:100%;font-size:0.9rem">
              <tr><td style="padding:0.3rem 0;color:var(--text-muted)">Versión</td><td>${pkg.version}</td></tr>
              <tr><td style="padding:0.3rem 0;color:var(--text-muted)">Licencia</td><td>${pkg.license || 'N/A'}</td></tr>
              <tr><td style="padding:0.3rem 0;color:var(--text-muted)">Autor</td><td>${pkg.author || 'N/A'}</td></tr>
              <tr><td style="padding:0.3rem 0;color:var(--text-muted)">Última actualización</td><td>${pkg.updated || 'N/A'}</td></tr>
              <tr><td style="padding:0.3rem 0;color:var(--text-muted)">Descargas</td><td>${this.formatNumber(pkg.downloads || 0)}</td></tr>
            </table>
          </div>
          <div class="card">
            <h3>Dependencias</h3>
            ${pkg.dependencies && pkg.dependencies.length > 0
              ? `<ul style="list-style:none;padding:0">${pkg.dependencies.map(d => `<li style="padding:0.2rem 0"><a href="?name=${encodeURIComponent(d)}">${d}</a></li>`).join('')}</ul>`
              : '<p style="color:var(--text-muted);font-size:0.9rem">Sin dependencias.</p>'
            }
          </div>
        </div>

        <div class="card">
          <h3>Instalación</h3>
          <pre><code>vexpm install ${pkg.name}</code></pre>
          <p style="margin-top:0.5rem;color:var(--text-secondary);font-size:0.85rem">
            Añade esta línea a tu archivo <code>vex.toml</code>:
          </p>
          <pre><code>${pkg.name} = "${pkg.version}"</code></pre>
        </div>

        <div style="margin-top:1rem">
          <a href="registry.html" class="btn">&larr; Volver al registro</a>
        </div>
      `;
    },

    formatNumber(n) {
      if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
      if (n >= 1000) return (n / 1000).toFixed(1) + 'K';
      return n.toString();
    },

    bindEvents() {
      const searchInput = document.getElementById('search-input');
      if (searchInput) {
        searchInput.addEventListener('input', (e) => {
          this.searchQuery = e.target.value;
          this.applyFilters();
        });
      }

      const sortSelect = document.getElementById('sort-select');
      if (sortSelect) {
        sortSelect.addEventListener('change', (e) => {
          this.currentSort = e.target.value;
          this.applyFilters();
        });
      }
    }
  };

  if (document.getElementById('registry-app')) {
    document.addEventListener('DOMContentLoaded', () => RegistryApp.init());
  }
})();
