SELECT seriesPeriodDetect([139, 87, 110, 68, 54, 50, 51, 53, 133, 86, 141, 97, 156, 94, 149, 95, 140, 77, 61, 50, 54, 47, 133, 72, 152, 94, 148, 105, 162, 101, 160, 87, 63, 53, 55, 54, 151, 103, 189, 108, 183, 113, 175, 113, 178, 90, 71, 62, 62, 65, 165, 109, 181, 115, 182, 121, 178, 114, 170]);
SELECT seriesPeriodDetect([10,20,30,10,20,30,10,20,30, 10,20,30,10,20,30,10,20,30,10,20,30]);
SELECT seriesPeriodDetect([10.1, 20.45, 40.34, 10.1, 20.45, 40.34,10.1, 20.45, 40.34,10.1, 20.45, 40.34,10.1, 20.45, 40.34,10.1, 20.45, 40.34,10.1, 20.45, 40.34, 10.1, 20.45, 40.34]);
SELECT seriesPeriodDetect([10.1, 10, 400, 10.1, 10, 400, 10.1, 10, 400,10.1, 10, 400,10.1, 10, 400,10.1, 10, 400,10.1, 10, 400,10.1, 10, 400]);
SELECT seriesPeriodDetect([1,2,3]);  -- { serverError BAD_ARGUMENTS}
SELECT seriesPeriodDetect(); --{ serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH}
SELECT seriesPeriodDetect([]); -- { serverError ILLEGAL_COLUMN}
SELECT seriesPeriodDetect([NULL, NULL, NULL]); -- { serverError ILLEGAL_COLUMN}
SELECT seriesPeriodDetect([10,20,30,10,202,30,NULL]); -- { serverError ILLEGAL_COLUMN }