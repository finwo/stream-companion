#!/usr/bin/env node

const fs      = require('fs');
const esbuild = require('esbuild');
const glob    = require('fast-glob');
const { EOL } = require("os");

const entryPoints = glob.sync('./src/main.ts')
  .sort()
  .reduce((r, a) => ({
    ...r,
    ...(
      a.substr(0,1) == '.' ?
        { [a.substr(6).substr(0,a.length - 9)]: __dirname + '/' + a } :
        { [a]: __dirname + `/node_modules/${a}.ts` }
    )
  }), {})

const config = {
  format: 'cjs',
  target: ['chrome108','firefox107'],
  mainFields: ['browser','module','main'],
  bundle: true,
  outdir: __dirname + '/dist',
  entryPoints: Object.values(entryPoints),
  minify: false,
  loader: {
    '.eot'  : 'dataurl',
    '.html' : 'text',
    '.svg'  : 'dataurl',
    '.ttf'  : 'dataurl',
    '.woff' : 'dataurl',
    '.woff2': 'dataurl',
  }
};

const buildList = [];
const styles    = ['global.css'];

esbuild
  .build(config)
  .then(() => {
    fs.copyFileSync(`./src/global.css`, `./dist/global.css`);

    for(const name of Object.keys(entryPoints)) {
      buildList.push(`${name}.js`);
      try {
        fs.statSync(config.outdir + `/${name}.css`);
        styles.push(`${name}.css`);
      } catch(e) {
        // Intentionally empty
      }
    }

    fs.writeFileSync(config.outdir + `/index.html`, `<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <style>* { box-sizing: border-box; }</style>
    ${styles.map(name => `<link rel="stylesheet" href="${name}"/>`).join(`${EOL}    `)}
  </head>
  <body>
    ${buildList.map(name => `<script defer src="${name}"></script>`).join(`${EOL}    `)}
  </body>
</html>
`);

    fs.writeFileSync(config.outdir + `/index.bundled.html`, `<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <style>* { box-sizing: border-box; }</style>
    ${styles.map(path => `<style type="text/css">${fs.readFileSync(`${config.outdir}/${path}`,'utf-8')}</style>`).join(`${EOL}    `)}
  </head>
  <body>
    ${buildList.map(path => `<script type="text/javascript">${fs.readFileSync(`${config.outdir}/${path}`,'utf-8').split("\r\n").join("\n").split("\r").join("\n").split("\n").join(EOL)}</script>`).join(`${EOL}    `)}
  </body>
</html>
`);

    console.log(fs.readFileSync(config.outdir + `/index.bundled.html`, 'utf-8'));

  })




