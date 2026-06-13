(function () {
  'use strict';

  const LS_KEY = 'vex-playground-code';
  const LS_EXAMPLE = 'vex-playground-example';

  const editor = document.getElementById('editor');
  const lineNumbers = document.getElementById('line-numbers');
  const outputEl = document.getElementById('output');
  const errorEl = document.getElementById('error');
  const runBtn = document.getElementById('run-btn');
  const clearBtn = document.getElementById('clear-btn');
  const statusEl = document.getElementById('status');
  const exampleList = document.getElementById('example-list');

  let currentExample = null;
  let worker = null;

  /* ---- Line numbers ---- */
  function updateLineNumbers() {
    const lines = editor.value.split('\n');
    const html = lines.map((_, i) => {
      const cls = 'ln-' + (i + 1);
      return '<span id="' + cls + '">' + (i + 1) + '</span>';
    }).join('');
    lineNumbers.innerHTML = html;
  }

  editor.addEventListener('input', updateLineNumbers);
  editor.addEventListener('scroll', function () {
    lineNumbers.scrollTop = this.scrollTop;
  });
  editor.addEventListener('keydown', function (e) {
    if (e.key === 'Tab') {
      e.preventDefault();
      const start = this.selectionStart;
      const end = this.selectionEnd;
      this.value = this.value.substring(0, start) + '    ' + this.value.substring(end);
      this.selectionStart = this.selectionEnd = start + 4;
      updateLineNumbers();
    }
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      runCode();
    }
  });

  /* ---- Examples ---- */
  function loadExamples() {
    fetch('examples.json')
      .then(function (r) { return r.json(); })
      .then(function (examples) {
        exampleList.innerHTML = '';
        examples.forEach(function (ex, idx) {
          var li = document.createElement('li');
          li.textContent = ex.name;
          li.title = ex.description;
          li.dataset.index = idx;
          li.addEventListener('click', function () {
            selectExample(idx, examples);
          });
          exampleList.appendChild(li);
        });

        var saved = localStorage.getItem(LS_EXAMPLE);
        if (saved !== null) {
          var si = parseInt(saved, 10);
          if (!isNaN(si) && si >= 0 && si < examples.length) {
            selectExample(si, examples);
            return;
          }
        }

        var savedCode = localStorage.getItem(LS_KEY);
        if (savedCode) {
          editor.value = savedCode;
          updateLineNumbers();
        } else {
          selectExample(0, examples);
        }
      })
      .catch(function (err) {
        console.error('Failed to load examples:', err);
      });
  }

  function selectExample(idx, examples) {
    var items = exampleList.querySelectorAll('li');
    items.forEach(function (li, i) {
      li.classList.toggle('active', i === idx);
    });
    currentExample = idx;
    var ex = examples[idx];
    if (ex) {
      editor.value = ex.code;
      updateLineNumbers();
      localStorage.setItem(LS_KEY, ex.code);
      localStorage.setItem(LS_EXAMPLE, String(idx));
    }
    errorEl.classList.add('hidden');
    errorEl.textContent = '';
  }

  /* ---- Run ---- */
  function runCode() {
    var code = editor.value;
    if (!code.trim()) return;

    localStorage.setItem(LS_KEY, code);
    runBtn.disabled = true;
    statusEl.textContent = 'running...';
    statusEl.className = 'running';
    errorEl.classList.add('hidden');
    errorEl.textContent = '';

    // TODO: In a real environment, this would invoke a WebWorker
    // that loads the Vex WASM compiler. For now, we simulate the
    // output display. When the WASM backend is available, replace
    // the body of this function with the worker dispatch below.

    /* --- WebWorker dispatch (uncomment when WASM ready) ---
    if (worker) {
      worker.terminate();
    }
    worker = new Worker('vex-worker.js');
    worker.onmessage = function (e) {
      var msg = e.data;
      if (msg.type === 'stdout') {
        appendOutput(msg.text);
      } else if (msg.type === 'stderr') {
        appendOutput(msg.text, true);
      } else if (msg.type === 'error') {
        showError(msg.message);
      } else if (msg.type === 'done') {
        runBtn.disabled = false;
        statusEl.textContent = msg.exitCode === 0 ? 'exit ' + msg.exitCode : 'error ' + msg.exitCode;
        statusEl.className = msg.exitCode === 0 ? 'success' : 'error';
      }
    };
    worker.onerror = function (e) {
      showError('Worker error: ' + e.message);
      runBtn.disabled = false;
      statusEl.textContent = 'error';
      statusEl.className = 'error';
    };
    worker.postMessage({ type: 'compile', code: code });
    */

    // Simulation mode
    appendOutput('Vex Playground (simulation)\n');
    appendOutput('Compiling... ');
    setTimeout(function () {
      appendOutput('done.\n');
      appendOutput('Running...\n');
      setTimeout(function () {
        simulateRun(code);
      }, 300);
    }, 200);
  }

  function simulateRun(code) {
    var lines = code.split('\n');
    var hasPrint = false;
    var hasMain = false;

    for (var i = 0; i < lines.length; i++) {
      var line = lines[i].trim();
      if (line.startsWith('//')) continue;
      if (line.startsWith('i32 main()') || line.startsWith('i64 main()')) {
        hasMain = true;
      }
      if (line.indexOf('println(') !== -1) {
        hasPrint = true;
        var m = line.match(/println\("([^"]*)"\)/);
        if (m) {
          appendOutput(m[1] + '\n');
        }
        var mv = line.match(/println\(([^)]+)\)/);
        if (mv && !line.match(/println\("/)) {
          appendOutput('<expr result>\n');
        }
      }
    }

    if (!hasMain) {
      showError('Error: no main() function found');
    } else {
      statusEl.textContent = 'exit 0';
      statusEl.className = 'success';
      if (!hasPrint) {
        appendOutput('(no output)\n');
      }
    }
    runBtn.disabled = false;
  }

  function appendOutput(text, isErr) {
    var span = document.createElement('span');
    span.textContent = text;
    if (isErr) {
      span.style.color = '#ff7b72';
    }
    outputEl.appendChild(span);
    outputEl.scrollTop = outputEl.scrollHeight;
  }

  function showError(msg) {
    errorEl.textContent = msg;
    errorEl.classList.remove('hidden');
    statusEl.textContent = 'error';
    statusEl.className = 'error';
    runBtn.disabled = false;
  }

  function clearOutput() {
    outputEl.innerHTML = '';
    errorEl.classList.add('hidden');
    errorEl.textContent = '';
    statusEl.textContent = '';
    statusEl.className = '';
  }

  /* ---- ANSI rendering ---- */
  function renderANSI(text) {
    var ansiRegex = /\x1b\[(\d+)(?:;(\d+))*m/g;
    var parts = [];
    var last = 0;
    var m;
    var cls = '';

    while ((m = ansiRegex.exec(text)) !== null) {
      if (m.index > last) {
        parts.push({ text: text.slice(last, m.index), cls: cls });
      }
      var code = parseInt(m[1], 10);
      if (code === 0) cls = '';
      else if (code >= 30 && code <= 37) cls = 'ansi-' + ['black','red','green','yellow','blue','magenta','cyan','white'][code - 30];
      else if (code >= 90 && code <= 97) cls = 'ansi-bright-' + ['black','red','green','yellow','blue','magenta','cyan','white'][code - 90];
      else if (code === 1) cls = 'ansi-bold';
      last = m.index + m[0].length;
    }
    if (last < text.length) {
      parts.push({ text: text.slice(last), cls: cls });
    }
    return parts;
  }

  /* ---- Events ---- */
  runBtn.addEventListener('click', runCode);
  clearBtn.addEventListener('click', clearOutput);

  /* ---- Init ---- */
  loadExamples();

  /* Expose for debugging */
  window.__vexPlayground = {
    runCode: runCode,
    clearOutput: clearOutput,
    setCode: function (c) { editor.value = c; updateLineNumbers(); }
  };
})();
