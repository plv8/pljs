import { parseArgs } from 'node:util';

const GLOBAL_OPTIONS = {
  'connection-string': { type: 'string' },
  host: { type: 'string' },
  port: { type: 'string' },
  dbname: { type: 'string' },
  username: { type: 'string' },
  password: { type: 'string' },
  json: { type: 'boolean', default: false },
  force: { type: 'boolean', default: false },
  sloppy: { type: 'boolean', default: false },
  as: { type: 'string' },
  help: { type: 'boolean', default: false },
};

function printUsage() {
  console.log(`Usage: pljs <command> [options]

Commands:
  install <target>   Install a module or package
  remove <target>    Remove a module or package
  list               List installed modules and packages

Global Options:
  --connection-string  PostgreSQL connection string
  --host               Database host
  --port               Database port
  --dbname             Database name
  --username           Database user
  --password           Database password
  --json               Output as JSON
  --force              Overwrite existing modules/packages
  --sloppy             Run installed CommonJS modules in sloppy mode
  --help               Show this help message`);
}

export async function run(argv) {
  const { values, positionals } = parseArgs({
    args: argv,
    options: GLOBAL_OPTIONS,
    allowPositionals: true,
    strict: false,
  });

  if (values.help || positionals.length === 0) {
    printUsage();
    return;
  }

  const [command, ...args] = positionals;

  switch (command) {
    case 'install': {
      const { install } = await import('./commands/install.js');
      return install(args, values);
    }
    case 'remove': {
      const { remove } = await import('./commands/remove.js');
      return remove(args, values);
    }
    case 'list': {
      const { list } = await import('./commands/list.js');
      return list(args, values);
    }
    default:
      console.error(`Unknown command: ${command}`);
      printUsage();
      process.exit(1);
  }
}
