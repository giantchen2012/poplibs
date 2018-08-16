const std::vector<std::vector<float>> in1 = {
  {},
  { 1.1},
  { 3.1,  3.3},
  { 5.1,  5.3,  5.5},
  { 7.1,  7.3,  7.5,  7.7},
  { 9.1,  9.3,  9.5,  9.7,  9.9},
  {  .1,   .3,   .5,   .7,   .9,  2.1,  2.3,  2.5,  2.7}
};

const std::vector<std::vector<float>> in2 = {
  {},
  { 2.0},
  { 4.0,  4.2},
  { 6.0,  6.2,  6.4},
  { 8.0,  8.2,  8.4,  8.6},
  {10.0, 10.2, 10.4, 10.6, 10.8},
  {  .2,   .4,   .6,   .8,  1.0,  2.2,  2.4,  2.6,  2.8}
};

const std::vector<std::vector<float>> expected = {
  {},
  { 1.1},
  { 4.0,  3.3},
  { 6.0,  5.3,  6.4},
  { 7.1,  8.2,  7.5,  8.6},
  { 9.1,  9.3, 10.4, 10.6,  9.9},
  {  .2,   .4,   .6,   .7,   .9,  2.1,  2.4,  2.5,  2.8}
};