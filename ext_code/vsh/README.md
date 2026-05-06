# VEL - Vesta VM Lenguaje

Extension para el lenguaje Vesta Lang (VestaL) de la maquina virtual Vesta.

## Features

- Syntax highlighting completo

## Instalación

1. F12 -> Extension Development -> Publish Extension
2. O instala desde `.vsix`

## Ejemplo

```vel
@SpaceAddress {
    @Name("anonymous"),
    @IniAddress(0x0000000000000000),
    @EndAddress(0xFFFFFFFFFFFFFFFF)
}

@Section {
    @Name("all"),
    @SpaceAddress("anonymous")
    @Align(0x1000)
}
code:
    push r15
    push r15w
    push rbp
```

### **7. Comandos VSIX:**

```bash
# Compilar extension
cd vel
npm install -g @vscode/vsce
vsce package

# Instalar
code --install-extension vel-0.0.1.vsix

# generar el vsic:
vsce package
```
