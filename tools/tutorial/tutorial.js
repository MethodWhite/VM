(function() {
  'use strict';

  const STORAGE_KEY = 'vex_tutorial_progress';
  const PERSIST_KEY = 'vex_tutorial_code';

  const lessons = [
    {
      id: 'hello',
      title: 'Hello World',
      description: 'Tu primer programa en Vex',
      explanation: `Bienvenido a Vex. Todo programa comienza con la función <code>main</code>. Usa <code>println</code> para imprimir texto en la consola.`,
      code: `fn main() {
    println("Hola, mundo!");
}`,
      expected: 'Hola, mundo!\n',
      exercise: {
        prompt: 'Completa el código para imprimir "Hola, Vex!":',
        template: 'fn main() {\n    println("__BLANK__");\n}',
        answer: 'Hola, Vex!'
      }
    },
    {
      id: 'variables',
      title: 'Variables y Tipos',
      description: 'let, mut, i32, f64, bool, string',
      explanation: `Las variables se declaran con <code>let</code>. Por defecto son inmutables. Usa <code>mut</code> para hacerlas mutables. Los tipos básicos son <code>i32</code>, <code>f64</code>, <code>bool</code> y <code>string</code>.`,
      code: `fn main() {
    let nombre: string = "Vex";
    let mut edad: i32 = 0;
    edad = 1;
    let activo: bool = true;
    let precio: f64 = 99.99;

    println(nombre);
    println(edad);
    println(activo);
    println(precio);
}`,
      expected: 'Vex\n1\ntrue\n99.99\n',
      exercise: {
        prompt: 'Declara una variable mutable de tipo i32 llamada "contador" con valor 0:',
        template: 'fn main() {\n    __BLANK__\n}',
        answer: 'let mut contador: i32 = 0;'
      }
    },
    {
      id: 'control',
      title: 'Control de Flujo',
      description: 'if, else, while, for',
      explanation: `Vex soporta las estructuras de control clásicas: <code>if</code>/<code>else</code>, <code>while</code> y <code>for</code>. No necesitas paréntesis alrededor de la condición.`,
      code: `fn main() {
    let x: i32 = 10;

    if x > 5 {
        println("x es mayor que 5");
    } else {
        println("x es menor o igual a 5");
    }

    let mut i: i32 = 0;
    while i < 3 {
        println(i);
        i = i + 1;
    }

    for let j in 0..3 {
        println(j);
    }
}`,
      expected: 'x es mayor que 5\n0\n1\n2\n0\n1\n2\n',
      exercise: {
        prompt: 'Completa el bucle for para imprimir los números del 0 al 4:',
        template: 'fn main() {\n    for let i in __BLANK__ {\n        println(i);\n    }\n}',
        answer: '0..5'
      }
    },
    {
      id: 'functions',
      title: 'Funciones',
      description: 'fn, params, return',
      explanation: `Las funciones se declaran con <code>fn</code>. Pueden tener parámetros y valores de retorno (con <code>-></code>). El <code>return</code> es explícito.`,
      code: `fn suma(a: i32, b: i32) -> i32 {
    return a + b;
}

fn main() {
    let resultado: i32 = suma(3, 4);
    println(resultado);
}`,
      expected: '7\n',
      exercise: {
        prompt: 'Completa la función que recibe un i32 y retorna el doble:',
        template: 'fn doble(x: i32) -> i32 {\n    return __BLANK__;\n}\n\nfn main() {\n    println(doble(5));\n}',
        answer: 'x * 2'
      }
    },
    {
      id: 'structs',
      title: 'Structs y Tipos',
      description: 'type, struct',
      explanation: `Con <code>type</code> defines un struct con campos. Crea instancias con sintaxis nominativa y accede a campos con <code>.</code>.`,
      code: `type Persona struct {
    nombre: string;
    edad: i32;
}

fn main() {
    let p: Persona = Persona{
        nombre: "Ana",
        edad: 30,
    };
    println(p.nombre);
    println(p.edad);
}`,
      expected: 'Ana\n30\n',
      exercise: {
        prompt: 'Completa la creación del struct Punto con campos x e y:',
        template: 'type Punto struct {\n    x: i32;\n    __BLANK__\n}\n\nfn main() {\n    let p: Punto = Punto{x: 1, y: 2};\n    println(p.x);\n}',
        answer: 'y: i32;'
      }
    },
    {
      id: 'arrays',
      title: 'Arrays y Slices',
      description: 'T[], push, size',
      explanation: `Los arrays se declaran con <code>T[]</code>. Usa <code>push</code> para agregar elementos y <code>size</code> para obtener la longitud.`,
      code: `fn main() {
    let mut lista: i32[] = [];
    lista.push(10);
    lista.push(20);
    lista.push(30);

    println(lista.size());

    for let v in lista {
        println(v);
    }
}`,
      expected: '3\n10\n20\n30\n',
      exercise: {
        prompt: 'Completa el código para agregar el número 5 al array:',
        template: 'fn main() {\n    let mut nums: i32[] = [];\n    __BLANK__\n    println(nums.size());\n}',
        answer: 'nums.push(5);'
      }
    },
    {
      id: 'match',
      title: 'Pattern Matching',
      description: 'match, case',
      explanation: `<code>match</code> permite hacer pattern matching exhaustivo. Cada <code>case</code> debe cubrir todas las posibilidades.`,
      code: `fn main() {
    let valor: i32 = 2;

    match valor {
        case 1 => { println("uno"); }
        case 2 => { println("dos"); }
        case _ => { println("otro"); }
    }
}`,
      expected: 'dos\n',
      exercise: {
        prompt: 'Completa el match para el caso 3:',
        template: 'fn main() {\n    let v: i32 = 3;\n    match v {\n        case 1 => { println("uno"); }\n        case 2 => { println("dos"); }\n        __BLANK__\n    }\n}',
        answer: 'case 3 => { println("tres"); }'
      }
    },
    {
      id: 'borrow',
      title: 'Borrow Checker',
      description: 'unique, borrow, lend',
      explanation: `Vex usa borrow checking para seguridad de memoria. <code>unique</code> indica propiedad única, <code>borrow</code> es referencia inmutable y <code>lend</code> es referencia mutable temporal.`,
      code: `fn main() {
    let unique dato: i32 = 42;
    let ref: &i32 = borrow dato;
    println(ref);
}`,
      expected: '42\n',
      exercise: {
        prompt: 'Usa "borrow" para crear una referencia a la variable "x":',
        template: 'fn main() {\n    let unique x: string = "hola";\n    let r: &string = __BLANK__\n    println(r);\n}',
        answer: 'borrow x;'
      }
    },
    {
      id: 'errors',
      title: 'Manejo de Errores',
      description: 'Result, Option',
      explanation: `Vex usa <code>Result&lt;T, E&gt;</code> y <code>Option&lt;T&gt;</code> para manejo de errores. <code>unwrap</code> extrae el valor o lanza error. <code>match</code> es la forma segura.`,
      code: `fn dividir(a: i32, b: i32) -> Result<i32, string> {
    if b == 0 {
        return Result::Err("division por cero");
    }
    return Result::Ok(a / b);
}

fn main() {
    let res: Result<i32, string> = dividir(10, 2);
    match res {
        case Result::Ok(v) => { println(v); }
        case Result::Err(e) => { println(e); }
    }
}`,
      expected: '5\n',
      exercise: {
        prompt: 'Completa el match para Option::Some:',
        template: 'fn main() {\n    let val: Option<i32> = Option::Some(7);\n    match val {\n        case Option::Some(v) => { println(v); }\n        __BLANK__\n    }\n}',
        answer: 'case Option::None => { println("nada"); }'
      }
    },
    {
      id: 'async',
      title: 'Async y Distribución',
      description: 'async, await, spawn',
      explanation: `Vex soporta concurrencia con <code>async</code>/<code>await</code> y <code>spawn</code> para tareas distribuidas. Las funciones async pueden ejecutarse en paralelo.`,
      code: `async fn tarea(id: i32) -> i32 {
    return id * 2;
}

fn main() {
    let fut = spawn tarea(5);
    let resultado = await fut;
    println(resultado);
}`,
      expected: '10\n',
      exercise: {
        prompt: 'Completa el spawn de la función async:',
        template: 'async fn saludar(nombre: string) -> string {\n    return "Hola " + nombre;\n}\n\nfn main() {\n    let fut = __BLANK__\n    let res = await fut;\n    println(res);\n}',
        answer: 'spawn saludar("Mundo");'
      }
    }
  ];

  const TutorialApp = {
    currentLesson: 0,
    progress: {},
    codeEditors: [],

    init() {
      this.loadProgress();
      this.loadLesson(this.getParam('lesson') || 0);
      this.bindEvents();
      this.renderProgressBar();
      this.renderLessonGrid();
    },

    getParam(key) {
      const params = new URLSearchParams(window.location.search);
      const v = params.get(key);
      return v ? parseInt(v, 10) : null;
    },

    loadProgress() {
      try {
        const saved = localStorage.getItem(STORAGE_KEY);
        this.progress = saved ? JSON.parse(saved) : {};
      } catch { this.progress = {}; }
    },

    saveProgress() {
      try { localStorage.setItem(STORAGE_KEY, JSON.stringify(this.progress)); } catch {}
    },

    markComplete(id) {
      this.progress[id] = { completed: true, completedAt: Date.now() };
      this.saveProgress();
      this.renderProgressBar();
      this.renderLessonGrid();
    },

    isComplete(id) { return !!(this.progress[id] && this.progress[id].completed); },

    loadLesson(index) {
      if (index < 0 || index >= lessons.length) return;
      this.currentLesson = index;
      const lesson = lessons[index];

      document.getElementById('lesson-title').textContent = `${index + 1}. ${lesson.title}`;
      document.getElementById('lesson-desc').textContent = lesson.description;
      document.getElementById('lesson-explanation').innerHTML = lesson.explanation;

      const codeEl = document.getElementById('lesson-code');
      codeEl.textContent = lesson.code;

      const editorEl = document.getElementById('code-editor');
      editorEl.value = this.getSavedCode(index) || lesson.code;

      const expectedEl = document.getElementById('expected-output');
      expectedEl.textContent = lesson.expected;

      document.getElementById('output-panel').textContent = '';
      document.getElementById('output-panel').className = '';

      document.getElementById('exercise-prompt').textContent = lesson.exercise.prompt;
      document.getElementById('exercise-input').value = '';
      document.getElementById('exercise-feedback').textContent = '';
      document.getElementById('exercise-feedback').className = '';

      document.getElementById('prev-btn').disabled = index === 0;
      document.getElementById('next-btn').disabled = index === lessons.length - 1;

      document.getElementById('lesson-counter').textContent = `${index + 1} / ${lessons.length}`;

      this.highlightSyntax();
      this.renderLessonGrid();
      window.scrollTo({ top: 0, behavior: 'smooth' });
    },

    getSavedCode(index) {
      try {
        const all = JSON.parse(localStorage.getItem(PERSIST_KEY) || '{}');
        return all[index] || null;
      } catch { return null; }
    },

    saveCode(index, code) {
      try {
        const all = JSON.parse(localStorage.getItem(PERSIST_KEY) || '{}');
        all[index] = code;
        localStorage.setItem(PERSIST_KEY, JSON.stringify(all));
      } catch {}
    },

    highlightSyntax() {
      const codeEl = document.getElementById('lesson-code');
      if (!codeEl) return;
      let html = codeEl.textContent;
      html = html
        .replace(/\b(fn|let|mut|return|if|else|while|for|match|case|type|struct|async|await|spawn|unique|borrow|lend|true|false)\b/g, '<span style="color:#c9d1d9;font-weight:600">$1</span>')
        .replace(/\b(i32|f64|bool|string|Result|Option)\b/g, '<span style="color:#58a6ff">$1</span>')
        .replace(/\/\/.*$/gm, '<span style="color:#8b949e">$&</span>')
        .replace(/"([^"]*)"/g, '<span style="color:#a5d6ff">"$1"</span>')
        .replace(/\b(\d+\.?\d*)\b/g, '<span style="color:#79c0ff">$1</span>');
      codeEl.innerHTML = html;
    },

    runCode() {
      const editor = document.getElementById('code-editor');
      const output = document.getElementById('output-panel');
      const code = editor.value.trim();

      this.saveCode(this.currentLesson, code);

      const lesson = lessons[this.currentLesson];
      const normalize = s => s.replace(/\r\n/g, '\n').replace(/\s+$/, '');

      if (normalize(code) === normalize(lesson.code)) {
        output.textContent = lesson.expected;
        output.className = 'alert alert-success';
        this.markComplete(lesson.id);
      } else {
        output.textContent = 'Ejecutando código... (simulado)\n\nNota: En un entorno real, esto compilaría y ejecutaría tu código Vex.\n\n' + lesson.expected;
        output.className = 'alert alert-info';
        this.markComplete(lesson.id);
      }
    },

    checkExercise() {
      const lesson = lessons[this.currentLesson];
      const input = document.getElementById('exercise-input').value.trim();
      const feedback = document.getElementById('exercise-feedback');

      if (input === lesson.exercise.answer) {
        feedback.textContent = 'Correcto!';
        feedback.className = 'alert alert-success';
        this.markComplete(lesson.id);
      } else {
        feedback.textContent = 'Incorrecto, intenta de nuevo.';
        feedback.className = 'alert alert-error';
      }
    },

    renderProgressBar() {
      const completed = lessons.filter(l => this.isComplete(l.id)).length;
      const pct = Math.round((completed / lessons.length) * 100);
      document.getElementById('progress-fill').style.width = pct + '%';
      document.getElementById('progress-text').textContent = `${completed} / ${lessons.length} lecciones (${pct}%)`;
    },

    renderLessonGrid() {
      const grid = document.getElementById('lesson-grid');
      if (!grid) return;
      grid.innerHTML = lessons.map((l, i) => {
        const active = i === this.currentLesson ? 'active' : '';
        const done = this.isComplete(l.id) ? 'done' : '';
        return `<div class="lesson-grid-item ${active} ${done}" data-index="${i}">
          <span class="lesson-grid-num">${i + 1}</span>
          <span class="lesson-grid-title">${l.title}</span>
          ${done ? '<span class="lesson-grid-check">✓</span>' : ''}
        </div>`;
      }).join('');
      grid.querySelectorAll('.lesson-grid-item').forEach(el => {
        el.addEventListener('click', () => {
          this.loadLesson(parseInt(el.dataset.index, 10));
        });
      });
    },

    bindEvents() {
      document.getElementById('prev-btn').addEventListener('click', () => this.loadLesson(this.currentLesson - 1));
      document.getElementById('next-btn').addEventListener('click', () => this.loadLesson(this.currentLesson + 1));
      document.getElementById('run-btn').addEventListener('click', () => this.runCode());
      document.getElementById('reset-btn').addEventListener('click', () => {
        const lesson = lessons[this.currentLesson];
        document.getElementById('code-editor').value = lesson.code;
        document.getElementById('output-panel').textContent = '';
        document.getElementById('output-panel').className = '';
        this.saveCode(this.currentLesson, lesson.code);
      });
      document.getElementById('check-exercise-btn').addEventListener('click', () => this.checkExercise());
      document.getElementById('exercise-input').addEventListener('keydown', (e) => {
        if (e.key === 'Enter') this.checkExercise();
      });
    }
  };

  if (document.getElementById('tutorial-app')) {
    document.addEventListener('DOMContentLoaded', () => TutorialApp.init());
  }
})();
