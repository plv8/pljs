-- ten seconds should be enough to show this doesn't destroy memory
set statement_timeout = '60s';
set pljs.memory_limit = '256';

do $$ Object.prototype [Symbol.iterator] = function() { return { next:() => this } };
[...({})];
$$ language pljs;

do $$ Object.prototype [Symbol.iterator] = function() { return { next:() => this } };
[...({})];
$$ language pljs;
