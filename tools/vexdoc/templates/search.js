/**
 * VexDoc - Client-side Search
 * Full-text search across documentation pages.
 */

(function () {
    'use strict';

    // ============================================================
    //  Theme Management
    // ============================================================

    function getTheme() {
        return localStorage.getItem('vexdoc-theme') || 'dark';
    }

    function setTheme(theme) {
        localStorage.setItem('vexdoc-theme', theme);
        if (theme === 'light') {
            document.documentElement.setAttribute('data-theme', 'light');
            document.getElementById('theme-icon').textContent = '\u2600';
        } else {
            document.documentElement.removeAttribute('data-theme');
            document.getElementById('theme-icon').textContent = '\u261E';
        }
    }

    window.toggleTheme = function () {
        const current = getTheme();
        setTheme(current === 'dark' ? 'light' : 'dark');
    };

    // Initialize theme
    setTheme(getTheme());

    // ============================================================
    //  Search Index
    // ============================================================

    let searchIndex = [];
    let searchWorker = null;

    /**
     * Build a search index from the current page content.
     * Indexes function names, type names, signatures, and descriptions.
     */
    function buildSearchIndex() {
        const index = [];

        // Index module name from h1
        const h1 = document.querySelector('.module-header h1');
        if (h1) {
            index.push({
                type: 'module',
                title: h1.textContent.trim(),
                text: h1.textContent.trim(),
                url: window.location.pathname
            });
        }

        // Index types
        document.querySelectorAll('.doc-type').forEach(function (el) {
            const nameEl = el.querySelector('.doc-type-name code');
            const kindEl = el.querySelector('.type-badge');
            const descEl = el.querySelector('.doc-description p');
            const name = nameEl ? nameEl.textContent.trim() : '';
            const kind = kindEl ? kindEl.textContent.trim() : '';
            const desc = descEl ? descEl.textContent.trim() : '';

            index.push({
                type: 'type',
                title: name + ' (' + kind + ')',
                text: name + ' ' + kind + ' ' + desc,
                url: window.location.pathname + '#type-' + name
            });

            // Index fields
            el.querySelectorAll('.doc-fields table tbody tr').forEach(function (row) {
                const cells = row.querySelectorAll('td');
                if (cells.length >= 2) {
                    const fieldName = cells[0].textContent.trim();
                    const fieldType = cells[1].textContent.trim();
                    const fieldDesc = cells[2] ? cells[2].textContent.trim() : '';
                    index.push({
                        type: 'field',
                        title: name + '.' + fieldName,
                        text: fieldName + ' ' + fieldType + ' ' + fieldDesc,
                        url: window.location.pathname + '#type-' + name
                    });
                }
            });
        });

        // Index functions
        document.querySelectorAll('.doc-function').forEach(function (el) {
            const sigEl = el.querySelector('.doc-signature code');
            const descEl = el.querySelector('.doc-description p');
            const id = el.id || '';
            const sig = sigEl ? sigEl.textContent.trim() : '';
            const desc = descEl ? descEl.textContent.trim() : '';

            // Extract function name from id
            const name = id.replace('fn-', '');
            index.push({
                type: 'function',
                title: name,
                text: sig + ' ' + desc,
                url: window.location.pathname + '#' + id
            });

            // Index parameters
            el.querySelectorAll('.doc-params table tbody tr').forEach(function (row) {
                const cells = row.querySelectorAll('td');
                if (cells.length >= 2) {
                    const paramName = cells[0].textContent.trim();
                    const paramType = cells[1].textContent.trim();
                    const paramDesc = cells[2] ? cells[2].textContent.trim() : '';
                    index.push({
                        type: 'param',
                        title: name + '(' + paramName + ')',
                        text: paramName + ' ' + paramType + ' ' + paramDesc,
                        url: window.location.pathname + '#fn-' + name
                    });
                }
            });
        });

        // Index constants
        document.querySelectorAll('.const-list li code').forEach(function (el) {
            const text = el.textContent.trim();
            index.push({
                type: 'constant',
                title: text,
                text: text,
                url: window.location.pathname
            });
        });

        return index;
    }

    /**
     * Simple fuzzy search score.
     * Returns a score where higher = better match.
     */
    function scoreMatch(query, text) {
        const lowerQuery = query.toLowerCase();
        const lowerText = text.toLowerCase();

        // Exact match
        if (lowerText === lowerQuery) return 100;

        // Contains match
        if (lowerText.indexOf(lowerQuery) >= 0) return 75;

        // Word boundary match
        const words = lowerQuery.split(/\s+/);
        let matchedWords = 0;
        for (let i = 0; i < words.length; i++) {
            if (words[i].length === 0) continue;
            if (lowerText.indexOf(words[i]) >= 0) {
                matchedWords++;
            }
        }
        if (matchedWords > 0) {
            return (matchedWords / words.length) * 50;
        }

        // Fuzzy character match
        let qi = 0;
        for (let ti = 0; ti < lowerText.length && qi < lowerQuery.length; ti++) {
            if (lowerText[ti] === lowerQuery[qi]) {
                qi++;
            }
        }
        if (qi === lowerQuery.length) {
            return 25;
        }

        return 0;
    }

    /**
     * Perform search across the index.
     */
    function performSearch(query) {
        if (!query || query.trim().length === 0) {
            return [];
        }

        const results = [];
        const seen = new Set();

        for (let i = 0; i < searchIndex.length; i++) {
            const item = searchIndex[i];
            const score = scoreMatch(query, item.text);
            if (score > 0 && !seen.has(item.url + item.title)) {
                seen.add(item.url + item.title);
                results.push({
                    score: score,
                    type: item.type,
                    title: item.title,
                    url: item.url
                });
            }
        }

        // Sort by score descending
        results.sort(function (a, b) {
            return b.score - a.score;
        });

        return results.slice(0, 20);
    }

    // ============================================================
    //  Search UI
    // ============================================================

    let searchDropdown = null;

    function createSearchDropdown() {
        if (searchDropdown) return;

        searchDropdown = document.createElement('div');
        searchDropdown.id = 'search-results';
        searchDropdown.style.cssText =
            'position: fixed; top: 60px; left: 20px; width: 260px; ' +
            'max-height: 400px; overflow-y: auto; ' +
            'background-color: var(--bg-secondary, #161b22); ' +
            'border: 1px solid var(--border-color, #30363d); ' +
            'border-radius: 8px; box-shadow: 0 8px 24px rgba(0,0,0,0.4); ' +
            'z-index: 1000; display: none;';
        document.body.appendChild(searchDropdown);
    }

    function showSearchResults(results) {
        createSearchDropdown();

        if (!results || results.length === 0) {
            searchDropdown.innerHTML =
                '<div style="padding: 16px; color: var(--text-muted, #6e7681); font-size: 0.875rem; text-align: center;">No results found</div>';
            searchDropdown.style.display = 'block';
            return;
        }

        let html = '';
        for (let i = 0; i < results.length; i++) {
            const r = results[i];
            const typeColors = {
                module: 'var(--accent-primary, #58a6ff)',
                type: 'var(--accent-purple, #bc8cff)',
                function: 'var(--accent-secondary, #3fb950)',
                field: 'var(--accent-warning, #d29922)',
                param: 'var(--text-muted, #6e7681)',
                constant: 'var(--accent-warning, #d29922)'
            };
            const color = typeColors[r.type] || 'var(--text-muted)';

            html += '<a href="' + r.url + '" style="' +
                'display: block; padding: 10px 16px; border-bottom: 1px solid var(--border-color, #30363d); ' +
                'text-decoration: none; color: var(--text-primary, #e6edf3); transition: background 0.15s;' +
                '" onmouseover="this.style.backgroundColor=\'var(--bg-hover, #252d3f)\'" ' +
                'onmouseout="this.style.backgroundColor=\'transparent\'" onclick="document.getElementById(\'search-input\').value=\'\'; this.closest(\'#search-results\').style.display=\'none\'">' +
                '<span style="display: inline-block; padding: 1px 6px; border-radius: 3px; font-size: 0.65rem; ' +
                'font-weight: 600; text-transform: uppercase; letter-spacing: 0.3px; ' +
                'background-color: ' + color + '22; color: ' + color + '; margin-right: 8px;">' +
                r.type + '</span>' +
                '<span style="font-size: 0.875rem;">' + escapeHtml(r.title) + '</span>' +
                '</a>';
        }
        searchDropdown.innerHTML = html;
        searchDropdown.style.display = 'block';
    }

    function hideSearchResults() {
        if (searchDropdown) {
            searchDropdown.style.display = 'none';
        }
    }

    function escapeHtml(text) {
        var div = document.createElement('div');
        div.appendChild(document.createTextNode(text));
        return div.innerHTML;
    }

    // ============================================================
    //  Public API
    // ============================================================

    window.searchDocs = function (query) {
        if (!query || query.trim().length === 0) {
            hideSearchResults();
            return;
        }

        // Build index if not yet built
        if (searchIndex.length === 0) {
            searchIndex = buildSearchIndex();
        }

        const results = performSearch(query);
        showSearchResults(results);
    };

    // Close search on escape
    document.addEventListener('keydown', function (e) {
        if (e.key === 'Escape') {
            hideSearchResults();
            document.getElementById('search-input').blur();
        }
    });

    // Close search on click outside
    document.addEventListener('click', function (e) {
        if (searchDropdown &&
            !e.target.closest('#search-results') &&
            !e.target.closest('#search-input')) {
            hideSearchResults();
        }
    });

    // Rebuild index on page load (dynamic content may have changed)
    document.addEventListener('DOMContentLoaded', function () {
        searchIndex = buildSearchIndex();
    });

})();
