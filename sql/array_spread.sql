-- we set the timeout to as high as we can, since some platforms may have slower CI machines
set pljs.statement_timeout = '65536s';
set pljs.memory_limit = '64';

do $$ Object.prototype [Symbol.iterator] = function() { return { next:() => this } };
[...({})];
$$ language pljs;

