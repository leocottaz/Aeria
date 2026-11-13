// Sert ui/ (et ce dossier sous /__test/) avec le faux hôte natif injecté dans index.html : la page croit tourner dans le dock.
//   node tools/test/mcp/page/mock_serve.js [port]     puis ouvrir http://127.0.0.1:8766/index.html
const http = require('http');
const fs = require('fs');
const path = require('path');

const ui = path.resolve(__dirname, '..', '..', '..', '..', 'ui');
const here = __dirname;
const port = Number(process.argv[2]) || 8766;
const types = { '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png' };

http.createServer((req, res) => {
  let p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (p === '/') p = '/index.html';
  const file = p.startsWith('/__test/') ? path.join(here, p.slice(8)) : path.join(ui, p);
  const root = p.startsWith('/__test/') ? here : ui;
  if (!path.resolve(file).startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
  let body = fs.readFileSync(file);
  if (p === '/index.html') body = Buffer.from(body.toString('utf8').replace('<head>', '<head><script src="/__test/mock_native.js"></script>'));
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
  res.end(body);
}).listen(port, '127.0.0.1', () => console.log(`http://127.0.0.1:${port}/index.html`));
