const AsyncFunction = Object.getPrototypeOf(async function(){}).constructor;

function gettype(subject): string {
  if (null === subject) return 'null';
  if (Array.isArray(subject)) return 'array';
  return typeof subject;
}

function flatten(subject, prefix = '') {
  const output = {};
  for(const key of Object.keys(subject)) {
    const value = subject[key];
    switch(gettype(value)) {
      case 'object':
      case 'array':
        output[prefix + key] = '[object Object]';
        Object.assign(output, flatten(value, prefix + key + '.'));
        break;
      case 'null':
        output[prefix + key] = 'null';
        break;
      case 'string':
        output[prefix + key] = value;
        break;
    }
  }
  return output;
}

async function renderString(template: string, data: any = {}): string {
  let output = template;
  let matches;

  // Replace with argument data
  const squished = flatten(data);
  for(const key of Object.keys(squished)) {
    output = output.replaceAll(`{{${key}}}`, squished[key]);
  }

  // Handle randInt command
  output = output.replace(/\{\{randInt (\d+)( \d+)?\}\}/g, (_, l, h) => {
    if ('undefined' === typeof h) { h = l; l = 0; };
    const minInt = Math.min(parseInt(l), parseInt(h));
    const maxInt = Math.max(parseInt(l), parseInt(h)) + 1;
    const diff   = maxInt - minInt;
    return minInt + Math.floor(Math.random() * diff);
  });

  // // Handle eval command
  // while(matches = output.match(/\{\{eval (.+?)\}\}/)) {
  //   const fn = new Function('return ' + matches[1]);
  //   output = output.replace(matches[0], fn());
  // }

  // Handle urlfetch command
  while(matches = output.match(/\{\{urlfetch (.+?)\}\}/)) {
    const result = await fetch(matches[1]);
    output = output.substring(0, matches.index) +
             (await result.text()) +
             output.substring(matches.index + matches[0].length);
  }

  return output;
}

export {
  renderString,
};
