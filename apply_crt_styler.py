import re
import html

html_file = 'specs/Introducing Lain_ A Systems Language with Linear Types and Capabilities.html'

with open(html_file, 'r', encoding='utf-8') as f:
    html_content = f.read()

# CSS to inject just before </style>
css_inject = """
    :root {
      --bg-color: #030204;
      --border-main: #333333;
      --border-active: #8b1e1e;
      --accent-purple: #c678dd;
      --accent-sepia: #e5c07b;
      --text-main: #dcdcdc;
      --text-dim: #888888;
    }

    body {
        background-color: var(--bg-color) !important;
        color: var(--text-main) !important;
    }

    a {
        color: var(--accent-sepia) !important;
    }
    
    h1, h2, h3, h4, h5, h6 {
        color: #ffffff;
        border-bottom-color: var(--border-main) !important;
    }

    /* CRT Effects */
    .crt-noise {
        position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; pointer-events: none; z-index: 50; opacity: 0.03;
        background: url("data:image/svg+xml,%3Csvg viewBox='0 0 200 200' xmlns='http://www.w3.org/2000/svg'%3E%3Cfilter id='noiseFilter'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='0.85' numOctaves='3' stitchTiles='stitch'/%3E%3C/filter%3E%3Crect width='100%25' height='100%25' filter='url(%23noiseFilter)'/%3E%3C/svg%3E");
    }
    .crt-scanlines {
        position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; pointer-events: none; z-index: 49;
        background: linear-gradient(to bottom, rgba(18, 16, 16, 0) 50%, rgba(0, 0, 0, 0.1) 50%); background-size: 100% 4px;
    }
    .crt-vignette {
        position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; pointer-events: none; z-index: 51;
        background: radial-gradient(circle, transparent 40%, rgba(0, 0, 0, 0.6) 150%);
    }
    .crt-flicker {
        position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; pointer-events: none; z-index: 52;
        background: rgba(18, 16, 16, 0.01); opacity: 0; animation: crtFlicker 0.1s infinite;
    }
    @keyframes crtFlicker { 0% { opacity: 0; } 50% { opacity: 0.02; } 100% { opacity: 0; } }

    /* Code styling */
    article pre {
        background-color: var(--bg-color) !important;
        border: 1px solid var(--border-main) !important;
        border-left: 2px solid var(--border-active) !important;
        padding: 1.5rem;
        position: relative;
        z-index: 100;
    }
    article pre>code {
        color: var(--text-main) !important;
    }
    .lain-kw { color: var(--accent-purple); font-weight: bold; } 
    .lain-type { color: var(--accent-sepia); } 
    .lain-str { color: var(--text-dim); font-style: italic; }
    .lain-cm { color: var(--text-dim); font-style: italic; }
"""

html_content = html_content.replace('</style>', css_inject + '\n</style>')

crt_divs = """
  <div class="crt-noise"></div>
  <div class="crt-scanlines"></div>
  <div class="crt-vignette"></div>
  <div class="crt-flicker"></div>
"""

# inject right after body tag
html_content = re.sub(r'<body[^>]*>', lambda m: m.group(0) + '\n' + crt_divs, html_content)

# Syntax highlighter words
keywords = {'func', 'proc', 'type', 'var', 'mov', 'borrow', 'mut', 'if', 'else', 'while', 'return', 'let', 'record', 'generic', 'module', 'end', 'is', 'begin', 'then', 'skip'}
primitive_types = {'int', 'string', 'bool', 'Unit'}

def highlight(code_text):
    code_text = html.unescape(code_text)
    
    tokens = []
    # Match comments, strings, identifiers, and symbols separately
    token_pat = re.compile(r'//.*|--.*|"[^"]*"|\b[a-zA-Z_]\w*\b|\s+|.')
    for m in token_pat.finditer(code_text):
        token = m.group(0)
        if token.startswith('//') or token.startswith('--'):
            tokens.append(f'<span class="lain-cm">{html.escape(token)}</span>')
        elif token.startswith('"'):
            tokens.append(f'<span class="lain-str">{html.escape(token)}</span>')
        elif token.isidentifier():
            if token in keywords:
                tokens.append(f'<span class="lain-kw">{token}</span>')
            elif token in primitive_types or (token and token[0].isupper()):
                tokens.append(f'<span class="lain-type">{token}</span>')
            else:
                tokens.append(token)
        else:
            tokens.append(html.escape(token))
            
    return ''.join(tokens)

# Find <pre><code> wrappers
pattern = re.compile(r'<pre(?:[^>]*)><code(?:[^>]*)>(.*?)</code></pre>', re.DOTALL)
def code_sub(m):
    return '<pre><code class="language-lain">' + highlight(m.group(1)) + '</code></pre>'

html_content = pattern.sub(code_sub, html_content)

with open(html_file, 'w', encoding='utf-8') as f:
    f.write(html_content)

print("done")
