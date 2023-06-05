DO $$ pljs.elog(NOTICE, 'this', 'is', 'inline', 'code') $$ LANGUAGE pljs;
DO $$ pljs.return_next(new Object());$$ LANGUAGE pljs;
