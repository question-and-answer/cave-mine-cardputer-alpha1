#include <M5Cardputer.h>
#include <math.h>

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

static constexpr int RENDER_W = 120;
static constexpr int RENDER_H = 67;
static constexpr int PIXEL_SCALE = 2;

static constexpr int WORLD_W = 48;
static constexpr int WORLD_H = 36;
static constexpr int WORLD_D = 48;

static constexpr int LUT_SIZE = 1024;
static constexpr int LUT_MASK = LUT_SIZE - 1;
static constexpr int LUT_SCALE = 16384;

static constexpr float FOV_X_DEG = 50.0f;
static constexpr float MAX_RAY_DIST = 34.0f;

static constexpr uint32_t FRAME_MS = 33;

static constexpr float PLAYER_WIDTH = 0.60f;
static constexpr float PLAYER_RADIUS = PLAYER_WIDTH * 0.5f;
static constexpr float PLAYER_HEIGHT = 1.80f;
static constexpr float EYE_HEIGHT = 1.62f;

static constexpr float WALK_SPEED = 4.35f;
static constexpr float AIR_SPEED_LIMIT = 1.10f;
static constexpr float GROUND_FRICTION = 42.0f;
static constexpr float GROUND_ACCEL = 48.0f;
static constexpr float AIR_ACCEL = 3.0f;
static constexpr float GRAVITY = 24.0f;
static constexpr float JUMP_SPEED = 8.7f;
static constexpr float MAX_FALL_SPEED = 20.0f;

static constexpr int YAW_STEP = 16;
static constexpr int PITCH_STEP = 12;
static constexpr float PLACE_REACH = 6.0f;

static constexpr int HOTBAR_SLOTS = 5;
static constexpr uint8_t hotbarBlocks[HOTBAR_SLOTS] = {
  1, 2, 3, 4, 5
};

static constexpr float FAR_TEXTURE_START = 10.0f;
static constexpr float FAR_TEXTURE_END   = 18.0f;

enum BlockType : uint8_t {
  BLOCK_AIR     = 0,
  BLOCK_GRASS   = 1,
  BLOCK_DIRT    = 2,
  BLOCK_STONE   = 3,
  BLOCK_OAK_LOG = 4,
  BLOCK_LEAVES  = 5
};

enum FaceType : uint8_t {
  FACE_X_NEG = 0,
  FACE_X_POS = 1,
  FACE_Y_NEG = 2,
  FACE_Y_POS = 3,
  FACE_Z_NEG = 4,
  FACE_Z_POS = 5
};

struct Vec3 {
  float x, y, z;
};

struct HitInfo {
  bool hit;
  uint8_t block;
  uint8_t face;
  int bx, by, bz;
  int prevX, prevY, prevZ;
  float dist;
  float hx, hy, hz;
};

uint8_t worldData[WORLD_W * WORLD_H * WORLD_D];
int16_t sinLUT[LUT_SIZE];

float rayScreenX[RENDER_W];
float rayScreenY[RENDER_H];
float tanHalfFovX = 0.0f;
float tanHalfFovY = 0.0f;

M5Canvas frameBuffer(&M5Cardputer.Display);

float playerX = 24.5f;
float playerY = 12.0f;
float playerZ = 24.5f;

float velX = 0.0f;
float velY = 0.0f;
float velZ = 0.0f;
bool grounded = false;

int yawIdx = 128;
int pitchIdx = 0;

int hotbarIndex = 0;
uint8_t selectedBlock = BLOCK_GRASS;

uint32_t lastFrameMs = 0;
uint32_t fpsTimerMs = 0;
uint16_t frameCounter = 0;
uint16_t shownFps = 0;

bool prevBreak = false;
bool prevPlace = false;
bool prevJump = false;

HitInfo targetHit;
bool haveTargetHit = false;

// -----------------------------------------------------------------------------
// World helpers
// -----------------------------------------------------------------------------

inline int worldIndex(int x, int y, int z) {
  return x + y * WORLD_W + z * WORLD_W * WORLD_H;
}

inline bool inBounds(int x, int y, int z) {
  return x >= 0 && x < WORLD_W &&
         y >= 0 && y < WORLD_H &&
         z >= 0 && z < WORLD_D;
}

inline uint8_t getBlock(int x, int y, int z) {
  if (!inBounds(x, y, z)) return BLOCK_STONE;
  return worldData[worldIndex(x, y, z)];
}

inline void setBlock(int x, int y, int z, uint8_t b) {
  if (!inBounds(x, y, z)) return;
  worldData[worldIndex(x, y, z)] = b;
}

inline bool isSolidBlock(uint8_t b) {
  return b != BLOCK_AIR;
}

// -----------------------------------------------------------------------------
// Trig LUT
// -----------------------------------------------------------------------------

inline int16_t sinQ(int idx) {
  return sinLUT[idx & LUT_MASK];
}

inline int16_t cosQ(int idx) {
  return sinLUT[(idx + LUT_SIZE / 4) & LUT_MASK];
}

inline float fsinLut(int idx) {
  return (float)sinQ(idx) / (float)LUT_SCALE;
}

inline float fcosLut(int idx) {
  return (float)cosQ(idx) / (float)LUT_SCALE;
}

void buildTrigLUT() {
  for (int i = 0; i < LUT_SIZE; ++i) {
    float a = (2.0f * PI * i) / (float)LUT_SIZE;
    sinLUT[i] = (int16_t)(sinf(a) * LUT_SCALE);
  }
}

// -----------------------------------------------------------------------------
// Hash / color
// -----------------------------------------------------------------------------

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

uint32_t hash2(int x, int z) {
  uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(z * 83492791);
  return hash32(h);
}

uint32_t hash3(int x, int y, int z) {
  uint32_t h = (uint32_t)(x * 73856093) ^
               (uint32_t)(y * 19349663) ^
               (uint32_t)(z * 83492791);
  return hash32(h);
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t shade565(uint16_t c, uint8_t shade) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5) & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;

  r = (uint8_t)((r * shade) >> 8);
  g = (uint8_t)((g * shade) >> 8);
  b = (uint8_t)((b * shade) >> 8);

  return rgb565(r, g, b);
}

uint16_t brighten565(uint16_t c, uint8_t add) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5) & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;

  r = (r + add > 255) ? 255 : r + add;
  g = (g + add > 255) ? 255 : g + add;
  b = (b + add > 255) ? 255 : b + add;

  return rgb565(r, g, b);
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

template <typename T>
bool keyHeld(const T& ks, char target) {
  for (char c : ks.word) {
    if (c == target || c == target - 32 || c == target + 32) return true;
  }
  return false;
}

template <typename T>
bool keyAnyDigit(const T& ks, char d) {
  for (char c : ks.word) {
    if (c == d) return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// Projection
// -----------------------------------------------------------------------------

void buildProjectionTables() {
  float aspect = (float)RENDER_W / (float)RENDER_H;
  float fovX = FOV_X_DEG * DEG_TO_RAD;
  tanHalfFovX = tanf(fovX * 0.5f);
  tanHalfFovY = tanHalfFovX / aspect;

  for (int x = 0; x < RENDER_W; ++x) {
    rayScreenX[x] = (((x + 0.5f) / (float)RENDER_W) * 2.0f - 1.0f);
  }
  for (int y = 0; y < RENDER_H; ++y) {
    rayScreenY[y] = (1.0f - ((y + 0.5f) / (float)RENDER_H) * 2.0f);
  }
}

void makeWorldRay(float sx, float sy, float &rx, float &ry, float &rz) {
  float lx = sx * tanHalfFovX;
  float ly = sy * tanHalfFovY;
  float lz = 1.0f;

  float invLen = 1.0f / sqrtf(lx * lx + ly * ly + lz * lz);
  lx *= invLen;
  ly *= invLen;
  lz *= invLen;

  float sp = fsinLut(pitchIdx);
  float cp = fcosLut(pitchIdx);
  float syaw = fsinLut(yawIdx);
  float cyaw = fcosLut(yawIdx);

  float px = lx;
  float py = ly * cp - lz * sp;
  float pz = ly * sp + lz * cp;

  rx = px * cyaw + pz * syaw;
  ry = py;
  rz = -px * syaw + pz * cyaw;
}

// -----------------------------------------------------------------------------
// 2D Perlin / fBM
// -----------------------------------------------------------------------------

static const uint8_t PERM[256] = {
  151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
  140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
  247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
  57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
  74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
  60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
  65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
  200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
  52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
  207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
  119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
  129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
  218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
  81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
  184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,
  222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

inline float fadef(float t) {
  return t * t * t * (t * (t * 6 - 15) + 10);
}

inline float lerpf(float a, float b, float t) {
  return a + t * (b - a);
}

inline float grad2(int hash, float x, float y) {
  switch (hash & 7) {
    case 0: return  x + y;
    case 1: return -x + y;
    case 2: return  x - y;
    case 3: return -x - y;
    case 4: return  x;
    case 5: return -x;
    case 6: return  y;
    default:return -y;
  }
}

float perlin2(float x, float y) {
  int xi = ((int)floorf(x)) & 255;
  int yi = ((int)floorf(y)) & 255;

  float xf = x - floorf(x);
  float yf = y - floorf(y);

  float u = fadef(xf);
  float v = fadef(yf);

  int aa = PERM[(PERM[xi] + yi) & 255];
  int ab = PERM[(PERM[xi] + yi + 1) & 255];
  int ba = PERM[(PERM[(xi + 1) & 255] + yi) & 255];
  int bb = PERM[(PERM[(xi + 1) & 255] + yi + 1) & 255];

  float x1 = lerpf(grad2(aa, xf, yf),     grad2(ba, xf - 1.0f, yf),     u);
  float x2 = lerpf(grad2(ab, xf, yf - 1), grad2(bb, xf - 1.0f, yf - 1), u);
  return lerpf(x1, x2, v);
}

float fbm2(float x, float y) {
  float sum = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;

  for (int i = 0; i < 4; ++i) {
    sum += perlin2(x * freq, y * freq) * amp;
    norm += amp;
    amp *= 0.5f;
    freq *= 2.0f;
  }
  return sum / norm;
}

// -----------------------------------------------------------------------------
// 3D value noise for caves
// -----------------------------------------------------------------------------

float rand01_3d(int x, int y, int z) {
  return (hash3(x, y, z) & 1023) / 1023.0f;
}

float valueNoise3D(float x, float y, float z) {
  int xi = (int)floorf(x);
  int yi = (int)floorf(y);
  int zi = (int)floorf(z);

  float xf = x - xi;
  float yf = y - yi;
  float zf = z - zi;

  float u = fadef(xf);
  float v = fadef(yf);
  float w = fadef(zf);

  float c000 = rand01_3d(xi,     yi,     zi);
  float c100 = rand01_3d(xi + 1, yi,     zi);
  float c010 = rand01_3d(xi,     yi + 1, zi);
  float c110 = rand01_3d(xi + 1, yi + 1, zi);
  float c001 = rand01_3d(xi,     yi,     zi + 1);
  float c101 = rand01_3d(xi + 1, yi,     zi + 1);
  float c011 = rand01_3d(xi,     yi + 1, zi + 1);
  float c111 = rand01_3d(xi + 1, yi + 1, zi + 1);

  float x00 = lerpf(c000, c100, u);
  float x10 = lerpf(c010, c110, u);
  float x01 = lerpf(c001, c101, u);
  float x11 = lerpf(c011, c111, u);

  float y0 = lerpf(x00, x10, v);
  float y1 = lerpf(x01, x11, v);

  return lerpf(y0, y1, w);
}

float fbm3(float x, float y, float z) {
  float sum = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;

  for (int i = 0; i < 3; ++i) {
    sum += valueNoise3D(x * freq, y * freq, z * freq) * amp;
    norm += amp;
    amp *= 0.5f;
    freq *= 2.0f;
  }
  return sum / norm;
}

// -----------------------------------------------------------------------------
// Terrain + trees
// -----------------------------------------------------------------------------

int terrainHeightAt(int x, int z) {
  float n1 = fbm2(x * 0.050f, z * 0.050f);
  float n2 = fbm2((x + 200) * 0.090f, (z - 100) * 0.090f);

  float h = 12.0f;
  h += n1 * 6.0f;
  h += n2 * 2.0f;

  int hi = (int)roundf(h);
  if (hi < 7) hi = 7;
  if (hi > WORLD_H - 8) hi = WORLD_H - 8;
  return hi;
}

bool carveCaveAt(int x, int y, int z, int surfaceY) {
  if (y >= surfaceY - 1) return false;
  if (y < 4) return false;

  float n1 = fbm3(x * 0.090f, y * 0.120f, z * 0.090f);
  float n2 = fbm3((x + 37) * 0.050f, (y + 11) * 0.070f, (z - 23) * 0.050f);
  float cave = n1 * 0.72f + n2 * 0.28f;

  float verticalBias = (y < surfaceY - 8) ? 0.0f : 0.06f;
  return cave > (0.69f + verticalBias);
}

int findTopSurfaceY(int x, int z) {
  for (int y = WORLD_H - 2; y >= 1; --y) {
    if (getBlock(x, y, z) == BLOCK_GRASS) return y;
  }
  return -1;
}

bool canPlaceTreeAt(int cx, int cy, int cz) {
  if (cx - 2 < 1 || cx + 2 >= WORLD_W - 1 ||
      cz - 2 < 1 || cz + 2 >= WORLD_D - 1 ||
      cy < 1 || cy + 6 >= WORLD_H - 1) {
    return false;
  }

  if (getBlock(cx, cy - 1, cz) != BLOCK_GRASS) return false;

  for (int y = 0; y <= 5; ++y) {
    for (int dz = -2; dz <= 2; ++dz) {
      for (int dx = -2; dx <= 2; ++dx) {
        uint8_t b = getBlock(cx + dx, cy + y, cz + dz);
        if (b != BLOCK_AIR && b != BLOCK_LEAVES) return false;
      }
    }
  }

  return true;
}

void placeLeafIfAir(int x, int y, int z) {
  if (inBounds(x, y, z) && getBlock(x, y, z) == BLOCK_AIR) {
    setBlock(x, y, z, BLOCK_LEAVES);
  }
}

void placeLeafRing5NoCorners(int cx, int cy, int cz) {
  for (int dz = -2; dz <= 2; ++dz) {
    for (int dx = -2; dx <= 2; ++dx) {
      if (dx == 0 && dz == 0) continue;
      if (abs(dx) == 2 && abs(dz) == 2) continue; // remove corners
      placeLeafIfAir(cx + dx, cy, cz + dz);
    }
  }
}

void generateTreeAt(int cx, int cy, int cz) {
  // Layer 1
  setBlock(cx, cy + 0, cz, BLOCK_OAK_LOG);

  // Layer 2
  setBlock(cx, cy + 1, cz, BLOCK_OAK_LOG);

  // Layer 3 and 4: 5x5 square with corners removed + center log
  for (int layer = 2; layer <= 3; ++layer) {
    setBlock(cx, cy + layer, cz, BLOCK_OAK_LOG);
    placeLeafRing5NoCorners(cx, cy + layer, cz);
  }

  // Layer 5: center log + full surrounding 3x3 corners and edges
  setBlock(cx, cy + 4, cz, BLOCK_OAK_LOG);
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dz == 0) continue;
      placeLeafIfAir(cx + dx, cy + 4, cz + dz);
    }
  }

  // Layer 6: leaves center + surrounding edges
  placeLeafIfAir(cx,     cy + 5, cz);
  placeLeafIfAir(cx - 1, cy + 5, cz);
  placeLeafIfAir(cx + 1, cy + 5, cz);
  placeLeafIfAir(cx,     cy + 5, cz - 1);
  placeLeafIfAir(cx,     cy + 5, cz + 1);
}

void generateTrees() {
  for (int z = 3; z < WORLD_D - 3; z += 5) {
    for (int x = 3; x < WORLD_W - 3; x += 5) {
      uint32_t h = hash2(x, z);
      if ((h & 3) != 0) continue;

      int jitterX = (int)((h >> 4) & 1) - (int)((h >> 5) & 1);
      int jitterZ = (int)((h >> 6) & 1) - (int)((h >> 7) & 1);

      int tx = x + jitterX;
      int tz = z + jitterZ;

      int surfaceY = findTopSurfaceY(tx, tz);
      if (surfaceY < 0) continue;

      int treeBaseY = surfaceY + 1;
      if (canPlaceTreeAt(tx, treeBaseY, tz)) {
        generateTreeAt(tx, treeBaseY, tz);
      }
    }
  }
}

void generateWorld() {
  for (int z = 0; z < WORLD_D; ++z) {
    for (int x = 0; x < WORLD_W; ++x) {
      int surface = terrainHeightAt(x, z);

      for (int y = 0; y < WORLD_H; ++y) {
        uint8_t b = BLOCK_AIR;

        if (y < surface - 4) b = BLOCK_STONE;
        else if (y < surface) b = BLOCK_DIRT;
        else if (y == surface) b = BLOCK_GRASS;

        if (b != BLOCK_AIR && carveCaveAt(x, y, z, surface)) {
          b = BLOCK_AIR;
        }

        setBlock(x, y, z, b);
      }
    }
  }

  // Spawn clearing
  for (int z = 22; z <= 27; ++z) {
    for (int y = 11; y <= 16; ++y) {
      for (int x = 22; x <= 27; ++x) {
        setBlock(x, y, z, BLOCK_AIR);
      }
    }
  }
  for (int z = 22; z <= 27; ++z) {
    for (int x = 22; x <= 27; ++x) {
      setBlock(x, 11, z, BLOCK_STONE);
    }
  }

  generateTrees();

  playerX = 24.5f;
  playerY = 12.0f;
  playerZ = 24.5f;
  velX = velY = velZ = 0.0f;
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

uint16_t getBlockBaseColor(uint8_t block, uint8_t face) {
  switch (block) {
    case BLOCK_GRASS:
      if (face == FACE_Y_POS) return rgb565(90, 170, 78);
      if (face == FACE_Y_NEG) return rgb565(95, 72, 48);
      return rgb565(100, 120, 72);

    case BLOCK_DIRT:
      return rgb565(112, 79, 50);

    case BLOCK_STONE:
      return rgb565(122, 126, 134);

    case BLOCK_OAK_LOG:
      if (face == FACE_Y_POS || face == FACE_Y_NEG) return rgb565(150, 120, 80);
      return rgb565(112, 82, 48);

    case BLOCK_LEAVES:
      return rgb565(56, 128, 52);

    default:
      return BLACK;
  }
}

uint8_t faceShade(uint8_t face) {
  switch (face) {
    case FACE_Y_POS: return 255;
    case FACE_Y_NEG: return 105;
    case FACE_X_NEG: return 180;
    case FACE_X_POS: return 160;
    case FACE_Z_NEG: return 205;
    case FACE_Z_POS: return 188;
    default: return 180;
  }
}

uint8_t textureDetailForDistance(float d) {
  if (d < FAR_TEXTURE_START) return 2;
  if (d < FAR_TEXTURE_END)   return 1;
  return 0;
}

uint16_t sampleBlockColor(const HitInfo &hit) {
  uint16_t base = getBlockBaseColor(hit.block, hit.face);
  uint8_t shade = faceShade(hit.face);

  float u = 0.0f;
  float v = 0.0f;

  switch (hit.face) {
    case FACE_X_NEG:
    case FACE_X_POS:
      u = hit.hz - floorf(hit.hz);
      v = hit.hy - floorf(hit.hy);
      break;
    case FACE_Y_NEG:
    case FACE_Y_POS:
      u = hit.hx - floorf(hit.hx);
      v = hit.hz - floorf(hit.hz);
      break;
    case FACE_Z_NEG:
    case FACE_Z_POS:
      u = hit.hx - floorf(hit.hx);
      v = hit.hy - floorf(hit.hy);
      break;
  }

  int tx = (int)(u * 8.0f) & 7;
  int ty = (int)(v * 8.0f) & 7;

  uint8_t detail = textureDetailForDistance(hit.dist);
  uint8_t tex = 0;

  if (detail > 0) {
    if (hit.block == BLOCK_STONE) {
      tex = (((tx ^ ty) & 1) ? 10 : 0);
      if (detail > 1) tex += (hash3(hit.bx + tx, hit.by + ty, hit.bz) & 4);
    } else if (hit.block == BLOCK_GRASS) {
      tex = (((tx + ty) & 1) ? 8 : 0);
    } else if (hit.block == BLOCK_DIRT) {
      tex = (((tx ^ (ty << 1)) & 1) ? 7 : 0);
    } else if (hit.block == BLOCK_OAK_LOG) {
      if (hit.face == FACE_Y_POS || hit.face == FACE_Y_NEG) {
        // end-grain rings
        int cx = tx - 4;
        int cy = ty - 4;
        int r2 = cx * cx + cy * cy;
        if (r2 < 4) tex = 8;
        else if (r2 < 10) tex = 18;
        else if (r2 < 18) tex = 8;
        else tex = 0;
      } else {
        tex = (((tx + ty) & 1) ? 12 : 0);
      }
    } else if (hit.block == BLOCK_LEAVES) {
      if (detail > 1) {
        tex = ((hash3(hit.bx + tx, hit.by + ty, hit.bz) & 7) < 3) ? 18 : 0;
      } else {
        tex = (((tx + ty) & 1) ? 8 : 0);
      }
    }
  }

  uint8_t finalShade = shade;
  if (tex < 255 - finalShade) finalShade += tex;

  if (hit.dist > 12.0f) {
    int fogDrop = (int)((hit.dist - 12.0f) * 7.0f);
    if (fogDrop > 110) fogDrop = 110;
    finalShade = (finalShade > fogDrop) ? (finalShade - fogDrop) : 18;
  }

  return shade565(base, finalShade);
}

bool traceVoxel(float ox, float oy, float oz,
                float dx, float dy, float dz,
                float maxDist,
                HitInfo &outHit) {
  int x = (int)floorf(ox);
  int y = (int)floorf(oy);
  int z = (int)floorf(oz);

  int stepX = (dx > 0.0f) ? 1 : -1;
  int stepY = (dy > 0.0f) ? 1 : -1;
  int stepZ = (dz > 0.0f) ? 1 : -1;

  float invDx = (fabsf(dx) > 0.00001f) ? (1.0f / dx) : 1e30f;
  float invDy = (fabsf(dy) > 0.00001f) ? (1.0f / dy) : 1e30f;
  float invDz = (fabsf(dz) > 0.00001f) ? (1.0f / dz) : 1e30f;

  float tDeltaX = fabsf(invDx);
  float tDeltaY = fabsf(invDy);
  float tDeltaZ = fabsf(invDz);

  float nextX = (dx > 0.0f) ? ((float)x + 1.0f - ox) : (ox - (float)x);
  float nextY = (dy > 0.0f) ? ((float)y + 1.0f - oy) : (oy - (float)y);
  float nextZ = (dz > 0.0f) ? ((float)z + 1.0f - oz) : (oz - (float)z);

  float tMaxX = (fabsf(dx) > 0.00001f) ? (nextX * tDeltaX) : 1e30f;
  float tMaxY = (fabsf(dy) > 0.00001f) ? (nextY * tDeltaY) : 1e30f;
  float tMaxZ = (fabsf(dz) > 0.00001f) ? (nextZ * tDeltaZ) : 1e30f;

  int prevX = x;
  int prevY = y;
  int prevZ = z;
  uint8_t enteredFace = FACE_Z_POS;
  float t = 0.0f;

  for (int steps = 0; steps < 128; ++steps) {
    if (inBounds(x, y, z)) {
      uint8_t b = getBlock(x, y, z);
      if (b != BLOCK_AIR) {
        outHit.hit = true;
        outHit.block = b;
        outHit.face = enteredFace;
        outHit.bx = x;
        outHit.by = y;
        outHit.bz = z;
        outHit.prevX = prevX;
        outHit.prevY = prevY;
        outHit.prevZ = prevZ;
        outHit.dist = t;
        outHit.hx = ox + dx * t;
        outHit.hy = oy + dy * t;
        outHit.hz = oz + dz * t;
        return true;
      }
    }

    prevX = x;
    prevY = y;
    prevZ = z;

    if (tMaxX < tMaxY) {
      if (tMaxX < tMaxZ) {
        x += stepX;
        t = tMaxX;
        tMaxX += tDeltaX;
        enteredFace = (stepX > 0) ? FACE_X_NEG : FACE_X_POS;
      } else {
        z += stepZ;
        t = tMaxZ;
        tMaxZ += tDeltaZ;
        enteredFace = (stepZ > 0) ? FACE_Z_NEG : FACE_Z_POS;
      }
    } else {
      if (tMaxY < tMaxZ) {
        y += stepY;
        t = tMaxY;
        tMaxY += tDeltaY;
        enteredFace = (stepY > 0) ? FACE_Y_NEG : FACE_Y_POS;
      } else {
        z += stepZ;
        t = tMaxZ;
        tMaxZ += tDeltaZ;
        enteredFace = (stepZ > 0) ? FACE_Z_NEG : FACE_Z_POS;
      }
    }

    if (t > maxDist) break;
    if (x < -1 || y < -1 || z < -1 || x > WORLD_W || y > WORLD_H || z > WORLD_D) break;
  }

  outHit.hit = false;
  return false;
}

void drawBackground() {
  uint16_t skyTop = rgb565(78, 126, 180);
  uint16_t skyBot = rgb565(120, 170, 210);
  uint16_t ground = rgb565(34, 30, 26);

  for (int py = 0; py < RENDER_H; ++py) {
    int sy = py * PIXEL_SCALE;
    uint16_t c;

    if (py < (RENDER_H / 2)) {
      uint8_t t = (uint8_t)((py * 255) / (RENDER_H / 2));
      uint8_t r = 78 + ((120 - 78) * t >> 8);
      uint8_t g = 126 + ((170 - 126) * t >> 8);
      uint8_t b = 180 + ((210 - 180) * t >> 8);
      c = rgb565(r, g, b);
    } else {
      c = ground;
    }

    frameBuffer.fillRect(0, sy, RENDER_W * PIXEL_SCALE, PIXEL_SCALE, c);
  }

  int usedH = RENDER_H * PIXEL_SCALE;
  if (usedH < SCREEN_H) {
    frameBuffer.fillRect(0, usedH, SCREEN_W, SCREEN_H - usedH, BLACK);
  }
}

bool sameTargetFace(const HitInfo &a, const HitInfo &b) {
  return a.hit && b.hit &&
         a.bx == b.bx && a.by == b.by && a.bz == b.bz &&
         a.face == b.face;
}

void updateTargetHit() {
  float camX = playerX;
  float camY = playerY + EYE_HEIGHT;
  float camZ = playerZ;

  float rx, ry, rz;
  makeWorldRay(0.0f, 0.0f, rx, ry, rz);
  haveTargetHit = traceVoxel(camX, camY, camZ, rx, ry, rz, PLACE_REACH, targetHit);
}

void renderFrame() {
  float camX = playerX;
  float camY = playerY + EYE_HEIGHT;
  float camZ = playerZ;

  frameBuffer.startWrite();
  drawBackground();

  for (int py = 0; py < RENDER_H; ++py) {
    int sy = py * PIXEL_SCALE;
    float ny = rayScreenY[py];

    for (int px = 0; px < RENDER_W; ++px) {
      float nx = rayScreenX[px];
      float rx, ry, rz;
      makeWorldRay(nx, ny, rx, ry, rz);

      HitInfo hit;
      if (traceVoxel(camX, camY, camZ, rx, ry, rz, MAX_RAY_DIST, hit)) {
        uint16_t c = sampleBlockColor(hit);

        if (haveTargetHit && sameTargetFace(hit, targetHit)) {
          c = brighten565(c, 42);
        }

        frameBuffer.fillRect(px * PIXEL_SCALE, sy, PIXEL_SCALE, PIXEL_SCALE, c);
      }
    }
  }

  int cx = SCREEN_W / 2;
  int cy = (RENDER_H * PIXEL_SCALE) / 2;
  frameBuffer.drawLine(cx - 4, cy, cx + 4, cy, WHITE);
  frameBuffer.drawLine(cx, cy - 4, cx, cy + 4, WHITE);

  frameBuffer.fillRect(0, 0, 160, 12, BLACK);
  frameBuffer.setCursor(2, 2);
  frameBuffer.setTextColor(WHITE, BLACK);
  frameBuffer.setTextSize(1);
  frameBuffer.printf("CAVE-MINE A FPS:%u", shownFps);

  int slotW = 22;
  int slotH = 16;
  int gap = 4;
  int totalW = HOTBAR_SLOTS * slotW + (HOTBAR_SLOTS - 1) * gap;
  int startX = (SCREEN_W - totalW) / 2;
  int y = SCREEN_H - slotH - 4;

  for (int i = 0; i < HOTBAR_SLOTS; ++i) {
    int x = startX + i * (slotW + gap);
    bool sel = (i == hotbarIndex);

    frameBuffer.fillRect(x, y, slotW, slotH, sel ? rgb565(55, 55, 55) : rgb565(20, 20, 20));
    frameBuffer.drawRect(x, y, slotW, slotH, sel ? WHITE : rgb565(110, 110, 110));

    uint16_t bc = getBlockBaseColor(hotbarBlocks[i], FACE_Y_POS);
    frameBuffer.fillRect(x + 5, y + 4, 12, 8, bc);

    char label[2] = { (char)('1' + i), '\0' };
    frameBuffer.setTextColor(WHITE, sel ? rgb565(55, 55, 55) : rgb565(20, 20, 20));
    frameBuffer.setCursor(x + 2, y + 2);
    frameBuffer.print(label);
  }

  frameBuffer.endWrite();
  frameBuffer.pushSprite(0, 0);
}

// -----------------------------------------------------------------------------
// Physics
// -----------------------------------------------------------------------------

bool isSolidAtCell(int x, int y, int z) {
  return isSolidBlock(getBlock(x, y, z));
}

bool aabbHitsWorld(float x, float footY, float z) {
  float minX = x - PLAYER_RADIUS;
  float maxX = x + PLAYER_RADIUS;
  float minY = footY;
  float maxY = footY + PLAYER_HEIGHT;
  float minZ = z - PLAYER_RADIUS;
  float maxZ = z + PLAYER_RADIUS;

  int x0 = (int)floorf(minX);
  int x1 = (int)floorf(maxX);
  int y0 = (int)floorf(minY);
  int y1 = (int)floorf(maxY);
  int z0 = (int)floorf(minZ);
  int z1 = (int)floorf(maxZ);

  for (int yy = y0; yy <= y1; ++yy) {
    for (int zz = z0; zz <= z1; ++zz) {
      for (int xx = x0; xx <= x1; ++xx) {
        if (isSolidAtCell(xx, yy, zz)) return true;
      }
    }
  }
  return false;
}

void solveCollisions(float dt) {
  grounded = false;

  float nextX = playerX + velX * dt;
  if (!aabbHitsWorld(nextX, playerY, playerZ)) playerX = nextX;
  else velX = 0.0f;

  float nextZ = playerZ + velZ * dt;
  if (!aabbHitsWorld(playerX, playerY, nextZ)) playerZ = nextZ;
  else velZ = 0.0f;

  float nextY = playerY + velY * dt;
  if (!aabbHitsWorld(playerX, nextY, playerZ)) {
    playerY = nextY;
  } else {
    if (velY < 0.0f) grounded = true;
    velY = 0.0f;
  }

  if (playerX < 1.0f) playerX = 1.0f;
  if (playerZ < 1.0f) playerZ = 1.0f;
  if (playerX > WORLD_W - 1.0f) playerX = WORLD_W - 1.0f;
  if (playerZ > WORLD_D - 1.0f) playerZ = WORLD_D - 1.0f;

  if (playerY < 1.0f) {
    playerY = 1.0f;
    velY = 0.0f;
    grounded = true;
  }

  float topClamp = WORLD_H - PLAYER_HEIGHT - 1.0f;
  if (playerY > topClamp) {
    playerY = topClamp;
    velY = 0.0f;
  }
}

void applyGroundFriction(float dt) {
  float speed = sqrtf(velX * velX + velZ * velZ);
  if (speed < 0.0001f) {
    velX = 0.0f;
    velZ = 0.0f;
    return;
  }

  float drop = GROUND_FRICTION * dt;
  float newSpeed = speed - drop;
  if (newSpeed < 0.0f) newSpeed = 0.0f;

  float scale = newSpeed / speed;
  velX *= scale;
  velZ *= scale;
}

void accelerateHorizontal(float wishX, float wishZ, float wishSpeed, float accel, float dt) {
  float currentSpeedAlongWish = velX * wishX + velZ * wishZ;
  float addSpeed = wishSpeed - currentSpeedAlongWish;
  if (addSpeed <= 0.0f) return;

  float accelSpeed = accel * dt;
  if (accelSpeed > addSpeed) accelSpeed = addSpeed;

  velX += accelSpeed * wishX;
  velZ += accelSpeed * wishZ;
}

// -----------------------------------------------------------------------------
// Game updates
// -----------------------------------------------------------------------------

template <typename T>
void updateLook(const T& ks) {
  if (keyHeld(ks, ',')) yawIdx = (yawIdx - YAW_STEP) & LUT_MASK;
  if (keyHeld(ks, '/')) yawIdx = (yawIdx + YAW_STEP) & LUT_MASK;

  if (keyHeld(ks, ';')) pitchIdx = (pitchIdx - PITCH_STEP) & LUT_MASK;
  if (keyHeld(ks, '.')) pitchIdx = (pitchIdx + PITCH_STEP) & LUT_MASK;

  int maxPitch = (LUT_SIZE / 4) - 6;
  if (pitchIdx > LUT_SIZE / 2) {
    int signedPitch = pitchIdx - LUT_SIZE;
    if (signedPitch < -maxPitch) pitchIdx = LUT_SIZE - maxPitch;
  } else {
    if (pitchIdx > maxPitch) pitchIdx = maxPitch;
  }
}

template <typename T>
void updateHotbarFromNumberKeys(const T& ks) {
  if (keyAnyDigit(ks, '1')) hotbarIndex = 0;
  else if (keyAnyDigit(ks, '2')) hotbarIndex = 1;
  else if (keyAnyDigit(ks, '3')) hotbarIndex = 2;
  else if (keyAnyDigit(ks, '4')) hotbarIndex = 3;
  else if (keyAnyDigit(ks, '5')) hotbarIndex = 4;

  selectedBlock = hotbarBlocks[hotbarIndex];
}

template <typename T>
void updateMovement(float dt, const T& ks) {
  float sy = fsinLut(yawIdx);
  float cy = fcosLut(yawIdx);

  float fx = sy;
  float fz = cy;
  float rx = cy;
  float rz = -sy;

  float wishX = 0.0f;
  float wishZ = 0.0f;

  if (keyHeld(ks, 'e')) { wishX += fx; wishZ += fz; }
  if (keyHeld(ks, 's')) { wishX -= fx; wishZ -= fz; }
  if (keyHeld(ks, 'a')) { wishX -= rx; wishZ -= rz; }
  if (keyHeld(ks, 'd')) { wishX += rx; wishZ += rz; }

  float wishLen = sqrtf(wishX * wishX + wishZ * wishZ);
  if (wishLen > 0.0001f) {
    wishX /= wishLen;
    wishZ /= wishLen;
  }

  if (grounded) {
    applyGroundFriction(dt);

    if (wishLen > 0.0001f) {
      accelerateHorizontal(wishX, wishZ, WALK_SPEED, GROUND_ACCEL, dt);

      float horizSpeed = sqrtf(velX * velX + velZ * velZ);
      if (horizSpeed > WALK_SPEED) {
        float scale = WALK_SPEED / horizSpeed;
        velX *= scale;
        velZ *= scale;
      }
    }
  } else {
    if (wishLen > 0.0001f) {
      accelerateHorizontal(wishX, wishZ, AIR_SPEED_LIMIT, AIR_ACCEL, dt);
    }
  }

  bool jumpNow = keyHeld(ks, ' ');
  if (jumpNow && !prevJump && grounded) {
    velY = JUMP_SPEED;
    grounded = false;
  }
  prevJump = jumpNow;

  velY -= GRAVITY * dt;
  if (velY < -MAX_FALL_SPEED) velY = -MAX_FALL_SPEED;

  solveCollisions(dt);
}

bool playerWouldOverlapCell(int bx, int by, int bz) {
  float minX = playerX - PLAYER_RADIUS;
  float maxX = playerX + PLAYER_RADIUS;
  float minY = playerY;
  float maxY = playerY + PLAYER_HEIGHT;
  float minZ = playerZ - PLAYER_RADIUS;
  float maxZ = playerZ + PLAYER_RADIUS;

  return !(bx + 1.0f <= minX || bx >= maxX ||
           by + 1.0f <= minY || by >= maxY ||
           bz + 1.0f <= minZ || bz >= maxZ);
}

template <typename T>
void handleBuildActions(const T& ks) {
  bool breakNow = keyHeld(ks, 'x');
  bool placeNow = keyHeld(ks, 'c');

  if (haveTargetHit && breakNow && !prevBreak) {
    setBlock(targetHit.bx, targetHit.by, targetHit.bz, BLOCK_AIR);
  }

  if (haveTargetHit && placeNow && !prevPlace) {
    int px = targetHit.prevX;
    int py = targetHit.prevY;
    int pz = targetHit.prevZ;

    if (inBounds(px, py, pz) &&
        getBlock(px, py, pz) == BLOCK_AIR &&
        !playerWouldOverlapCell(px, py, pz)) {
      setBlock(px, py, pz, selectedBlock);
    }
  }

  prevBreak = breakNow;
  prevPlace = placeNow;
}

void updateFps() {
  frameCounter++;
  uint32_t now = millis();
  if (now - fpsTimerMs >= 1000) {
    shownFps = frameCounter;
    frameCounter = 0;
    fpsTimerMs = now;
  }
}

// -----------------------------------------------------------------------------
// Boot / main
// -----------------------------------------------------------------------------

void drawBootScreen() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(10, 15);
  M5Cardputer.Display.println("CAVE-MINE");
  M5Cardputer.Display.setCursor(10, 30);
  M5Cardputer.Display.println("Alpha release build");
  M5Cardputer.Display.setCursor(10, 45);
  M5Cardputer.Display.println("Generating world...");
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(BLACK);

  Serial.begin(115200);
  Serial.println();
  Serial.println("CAVE-MINE alpha boot");

  buildTrigLUT();
  buildProjectionTables();
  drawBootScreen();
  generateWorld();

  frameBuffer.setColorDepth(16);
  if (!frameBuffer.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("createSprite failed");
    M5Cardputer.Display.setCursor(10, 65);
    M5Cardputer.Display.println("Sprite alloc failed");
  }

  selectedBlock = hotbarBlocks[hotbarIndex];

  fpsTimerMs = millis();
  lastFrameMs = millis();
}

void loop() {
  M5Cardputer.update();

  uint32_t now = millis();
  if (now - lastFrameMs < FRAME_MS) return;

  float dt = (now - lastFrameMs) * 0.001f;
  if (dt > 0.05f) dt = 0.05f;
  lastFrameMs = now;

  auto ks = M5Cardputer.Keyboard.keysState();

  updateLook(ks);
  updateHotbarFromNumberKeys(ks);
  updateMovement(dt, ks);
  updateTargetHit();
  handleBuildActions(ks);
  renderFrame();
  updateFps();
}