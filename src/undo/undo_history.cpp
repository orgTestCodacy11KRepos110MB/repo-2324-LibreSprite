/* ASE - Allegro Sprite Editor
 * Copyright (C) 2001-2011  David Capello
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include "undo/undo_history.h"

#include "base/memory.h"
#include "context.h"
#include "document.h"
#include "documents.h"
#include "raster/cel.h"
#include "raster/dirty.h"
#include "raster/image.h"
#include "raster/layer.h"
#include "raster/mask.h"
#include "raster/palette.h"
#include "raster/sprite.h"
#include "raster/stock.h"
#include "ui_context.h"
#include "undo/objects_container.h"

#include <allegro/config.h>
#include <errno.h>
#include <limits.h>
#include <list>
#include <stdio.h>
#include <vector>

// Undo state
enum {
  DO_UNDO,
  DO_REDO,
};

// Undo types
enum {
  // group
  UNDO_TYPE_OPEN,
  UNDO_TYPE_CLOSE,

  // data management
  UNDO_TYPE_DATA,

  // image management
  UNDO_TYPE_IMAGE,
  UNDO_TYPE_FLIP,
  UNDO_TYPE_DIRTY,

  // stock management
  UNDO_TYPE_ADD_IMAGE,
  UNDO_TYPE_REMOVE_IMAGE,
  UNDO_TYPE_REPLACE_IMAGE,

  // cel management
  UNDO_TYPE_ADD_CEL,
  UNDO_TYPE_REMOVE_CEL,

  // layer management
  UNDO_TYPE_SET_LAYER_NAME,
  UNDO_TYPE_ADD_LAYER,
  UNDO_TYPE_REMOVE_LAYER,
  UNDO_TYPE_MOVE_LAYER,
  UNDO_TYPE_SET_LAYER,

  // palette management
  UNDO_TYPE_ADD_PALETTE,
  UNDO_TYPE_REMOVE_PALETTE,
  UNDO_TYPE_SET_PALETTE_COLORS,
  UNDO_TYPE_REMAP_PALETTE,

  // misc
  UNDO_TYPE_SET_MASK,
  UNDO_TYPE_SET_IMGTYPE,
  UNDO_TYPE_SET_SIZE,
  UNDO_TYPE_SET_FRAME,
  UNDO_TYPE_SET_FRAMES,
  UNDO_TYPE_SET_FRLEN,
};

struct UndoChunkData;
struct UndoChunkImage;
struct UndoChunkFlip;
struct UndoChunkDirty;
struct UndoChunkAddImage;
struct UndoChunkRemoveImage;
struct UndoChunkReplaceImage;
struct UndoChunkAddCel;
struct UndoChunkRemoveCel;
struct UndoChunkSetLayerName;
struct UndoChunkAddLayer;
struct UndoChunkRemoveLayer;
struct UndoChunkMoveLayer;
struct UndoChunkSetLayer;
struct UndoChunkAddPalette;
struct UndoChunkRemovePalette;
struct UndoChunkSetPaletteColors;
struct UndoChunkRemapPalette;
struct UndoChunkSetMask;
struct UndoChunkSetImgType;
struct UndoChunkSetSize;
struct UndoChunkSetFrame;
struct UndoChunkSetFrames;
struct UndoChunkSetFrlen;

struct UndoChunk
{
  int type;
  int size;
  const char *label;
};

static UndoChunk* undo_chunk_new(UndoStream* stream, int type, int size);
static void undo_chunk_free(UndoChunk* chunk);

typedef std::list<UndoChunk*> ChunksList;

#include "undo_stream.h"

struct UndoAction
{
  const char *name;
  void (*invert)(UndoStream* stream, UndoChunk* chunk);
};

static int count_undo_groups(UndoStream* undo_stream);
static bool out_of_group(UndoStream* undo_stream);

// Undo actions

static void chunk_open_new(UndoStream* stream);
static void chunk_open_invert(UndoStream* stream, UndoChunk* chunk);

static void chunk_close_new(UndoStream* stream);
static void chunk_close_invert(UndoStream* stream, UndoChunk* chunk);

static void chunk_data_new(UndoStream* stream, void* object, void* fieldAddress, int fieldSize);
static void chunk_data_invert(UndoStream* stream, UndoChunkData *chunk);

static void chunk_image_new(UndoStream* stream, Image* image, int x, int y, int w, int h);
static void chunk_image_invert(UndoStream* stream, UndoChunkImage* chunk);

static void chunk_flip_new(UndoStream* stream, Image* image, int x1, int y1, int x2, int y2, bool horz);
static void chunk_flip_invert(UndoStream* stream, UndoChunkFlip *chunk);

static void chunk_dirty_new(UndoStream* stream, Image* image, Dirty *dirty);
static void chunk_dirty_invert(UndoStream* stream, UndoChunkDirty *chunk);

static void chunk_add_image_new(UndoStream* stream, Stock *stock, int image_index);
static void chunk_add_image_invert(UndoStream* stream, UndoChunkAddImage* chunk);

static void chunk_remove_image_new(UndoStream* stream, Stock *stock, int image_index);
static void chunk_remove_image_invert(UndoStream* stream, UndoChunkRemoveImage* chunk);

static void chunk_replace_image_new(UndoStream* stream, Stock *stock, int image_index);
static void chunk_replace_image_invert(UndoStream* stream, UndoChunkReplaceImage* chunk);

static void chunk_add_cel_new(UndoStream* stream, Layer* layer, Cel* cel);
static void chunk_add_cel_invert(UndoStream* stream, UndoChunkAddCel* chunk);

static void chunk_remove_cel_new(UndoStream* stream, Layer* layer, Cel* cel);
static void chunk_remove_cel_invert(UndoStream* stream, UndoChunkRemoveCel* chunk);

static void chunk_set_layer_name_new(UndoStream* stream, Layer *layer);
static void chunk_set_layer_name_invert(UndoStream* stream, UndoChunkSetLayerName* chunk);

static void chunk_add_layer_new(UndoStream* stream, Layer* set, Layer* layer);
static void chunk_add_layer_invert(UndoStream* stream, UndoChunkAddLayer* chunk);

static void chunk_remove_layer_new(UndoStream* stream, Layer* layer);
static void chunk_remove_layer_invert(UndoStream* stream, UndoChunkRemoveLayer* chunk);

static void chunk_move_layer_new(UndoStream* stream, Layer* layer);
static void chunk_move_layer_invert(UndoStream* stream, UndoChunkMoveLayer* chunk);

static void chunk_set_layer_new(UndoStream* stream, Sprite *sprite);
static void chunk_set_layer_invert(UndoStream* stream, UndoChunkSetLayer* chunk);

static void chunk_add_palette_new(UndoStream* stream, Sprite *sprite, Palette* palette);
static void chunk_add_palette_invert(UndoStream* stream, UndoChunkAddPalette *chunk);

static void chunk_remove_palette_new(UndoStream* stream, Sprite *sprite, Palette* palette);
static void chunk_remove_palette_invert(UndoStream* stream, UndoChunkRemovePalette *chunk);

static void chunk_set_palette_colors_new(UndoStream* stream, Sprite *sprite, Palette* palette, int from, int to);
static void chunk_set_palette_colors_invert(UndoStream* stream, UndoChunkSetPaletteColors *chunk);

static void chunk_remap_palette_new(UndoStream* stream, Sprite* sprite, int frame_from, int frame_to, const std::vector<int>& mapping);
static void chunk_remap_palette_invert(UndoStream* stream, UndoChunkRemapPalette *chunk);

static void chunk_set_mask_new(UndoStream* stream, Document* document);
static void chunk_set_mask_invert(UndoStream* stream, UndoChunkSetMask* chunk);

static void chunk_set_imgtype_new(UndoStream* stream, Sprite *sprite);
static void chunk_set_imgtype_invert(UndoStream* stream, UndoChunkSetImgType *chunk);

static void chunk_set_size_new(UndoStream* stream, Sprite *sprite);
static void chunk_set_size_invert(UndoStream* stream, UndoChunkSetSize *chunk);

static void chunk_set_frame_new(UndoStream* stream, Sprite *sprite);
static void chunk_set_frame_invert(UndoStream* stream, UndoChunkSetFrame *chunk);

static void chunk_set_frames_new(UndoStream* stream, Sprite *sprite);
static void chunk_set_frames_invert(UndoStream* stream, UndoChunkSetFrames *chunk);

static void chunk_set_frlen_new(UndoStream* stream, Sprite *sprite, int frame);
static void chunk_set_frlen_invert(UndoStream* stream, UndoChunkSetFrlen *chunk);

#define DECL_UNDO_ACTION(name) \
  { #name, (void (*)(UndoStream* , UndoChunk*))chunk_##name##_invert }

// WARNING: in the same order as in UNDO_TYPEs
static UndoAction undo_actions[] = {
  DECL_UNDO_ACTION(open),
  DECL_UNDO_ACTION(close),
  DECL_UNDO_ACTION(data),
  DECL_UNDO_ACTION(image),
  DECL_UNDO_ACTION(flip),
  DECL_UNDO_ACTION(dirty),
  DECL_UNDO_ACTION(add_image),
  DECL_UNDO_ACTION(remove_image),
  DECL_UNDO_ACTION(replace_image),
  DECL_UNDO_ACTION(add_cel),
  DECL_UNDO_ACTION(remove_cel),
  DECL_UNDO_ACTION(set_layer_name),
  DECL_UNDO_ACTION(add_layer),
  DECL_UNDO_ACTION(remove_layer),
  DECL_UNDO_ACTION(move_layer),
  DECL_UNDO_ACTION(set_layer),
  DECL_UNDO_ACTION(add_palette),
  DECL_UNDO_ACTION(remove_palette),
  DECL_UNDO_ACTION(set_palette_colors),
  DECL_UNDO_ACTION(remap_palette),
  DECL_UNDO_ACTION(set_mask),
  DECL_UNDO_ACTION(set_imgtype),
  DECL_UNDO_ACTION(set_size),
  DECL_UNDO_ACTION(set_frame),
  DECL_UNDO_ACTION(set_frames),
  DECL_UNDO_ACTION(set_frlen),
};

// Raw data

static Dirty *read_raw_dirty(uint8_t* raw_data);
static uint8_t* write_raw_dirty(uint8_t* raw_data, Dirty* dirty);
static int get_raw_dirty_size(Dirty *dirty);

static Image* read_raw_image(ObjectsContainer* objects, uint8_t* raw_data);
static uint8_t* write_raw_image(ObjectsContainer* objects, uint8_t* raw_data, Image* image);
static int get_raw_image_size(Image* image);

static Cel* read_raw_cel(ObjectsContainer* objects, uint8_t* raw_data);
static uint8_t* write_raw_cel(ObjectsContainer* objects, uint8_t* raw_data, Cel* cel);
static int get_raw_cel_size(Cel* cel);

static Layer* read_raw_layer(ObjectsContainer* objects, uint8_t* raw_data);
static uint8_t* write_raw_layer(ObjectsContainer* objects, uint8_t* raw_data, Layer* layer);
static int get_raw_layer_size(Layer* layer);

static Palette* read_raw_palette(uint8_t* raw_data);
static uint8_t* write_raw_palette(uint8_t* raw_data, Palette* palette);
static int get_raw_palette_size(Palette* palette);

static Mask* read_raw_mask(uint8_t* raw_data);
static uint8_t* write_raw_mask(uint8_t* raw_data, Mask* mask);
static int get_raw_mask_size(Mask* mask);

//////////////////////////////////////////////////////////////////////

UndoHistory::UndoHistory(ObjectsContainer* objects)
  : m_objects(objects)
{
  m_diffCount = 0;
  m_diffSaved = 0;
  m_enabled = true;
  m_label = NULL;

  m_undoStream = new UndoStream(this);
  try {
    m_redoStream = new UndoStream(this);
  }
  catch (...) {
    delete m_undoStream;
    throw;
  }
}

UndoHistory::~UndoHistory()
{
  delete m_undoStream;
  delete m_redoStream;
}

bool UndoHistory::isEnabled() const
{
  return m_enabled ? true: false;
}

void UndoHistory::setEnabled(bool state)
{
  m_enabled = state;
}

bool UndoHistory::canUndo() const
{
  return !m_undoStream->empty();
}

bool UndoHistory::canRedo() const
{
  return !m_redoStream->empty();
}

void UndoHistory::doUndo()
{
  runUndo(DO_UNDO);
}

void UndoHistory::doRedo()
{
  runUndo(DO_REDO);
}

void UndoHistory::clearRedo()
{
  if (!m_redoStream->empty())
    m_redoStream->clear();
}

const char* UndoHistory::getLabel()
{
  return m_label;
}

void UndoHistory::setLabel(const char* label)
{
  m_label = label;
}

const char* UndoHistory::getNextUndoLabel() const
{
  UndoChunk* chunk;

  ASSERT(canUndo());

  chunk = *m_undoStream->begin();
  return chunk->label;
}

const char* UndoHistory::getNextRedoLabel() const
{
  UndoChunk* chunk;

  ASSERT(canRedo());

  chunk = *m_redoStream->begin();
  return chunk->label;
}

bool UndoHistory::isSavedState() const
{
  return (m_diffCount == m_diffSaved);
}

void UndoHistory::markSavedState()
{
  m_diffSaved = m_diffCount;
}

void UndoHistory::runUndo(int state)
{
  UndoStream* undo_stream = ((state == DO_UNDO)? m_undoStream:
						 m_redoStream);
  UndoStream* redo_stream = ((state == DO_REDO)? m_undoStream:
						 m_redoStream);
  UndoChunk* chunk;
  int level = 0;

  do {
    chunk = undo_stream->popChunk(false); // read from head
    if (!chunk)
      break;

    setLabel(chunk->label);
    (undo_actions[chunk->type].invert)(redo_stream, chunk);

    if (chunk->type == UNDO_TYPE_OPEN)
      level++;
    else if (chunk->type == UNDO_TYPE_CLOSE)
      level--;

    undo_chunk_free(chunk);

    if (state == DO_UNDO)
      m_diffCount--;
    else if (state == DO_REDO)
      m_diffCount++;
  } while (level);
}

void UndoHistory::discardTail()
{
  UndoStream* undo_stream = m_undoStream;
  UndoChunk* chunk;
  int level = 0;

  do {
    chunk = undo_stream->popChunk(true); // read from tail
    if (!chunk)
      break;

    if (chunk->type == UNDO_TYPE_OPEN)
      level++;
    else if (chunk->type == UNDO_TYPE_CLOSE)
      level--;

    undo_chunk_free(chunk);
  } while (level);
}

static int count_undo_groups(UndoStream* undo_stream)
{
  UndoChunk* chunk;
  int groups = 0;
  int level;

  ChunksList::iterator it = undo_stream->begin();
  while (it != undo_stream->end()) {
    level = 0;

    do {
      chunk = *it;
      ++it;

      if (chunk->type == UNDO_TYPE_OPEN)
	level++;
      else if (chunk->type == UNDO_TYPE_CLOSE)
	level--;
    } while (level && (it != undo_stream->end()));

    if (level == 0)
      groups++;
  }

  return groups;
}

static bool out_of_group(UndoStream* undo_stream)
{
  UndoChunk* chunk;
  int level = 0;

  ChunksList::iterator it = undo_stream->begin();
  while (it != undo_stream->end()) {
    level = 0;

    do {
      chunk = *it;
      ++it;

      if (chunk->type == UNDO_TYPE_OPEN)
	level++;
      else if (chunk->type == UNDO_TYPE_CLOSE)
	level--;
    } while (level && (it != undo_stream->end()));
  }

  return level == 0;
}

// Called every time a new undo is added.
void UndoHistory::updateUndo()
{
  // TODO replace this with the following implementation:
  // * Add the undo limit to Undo class as a normal member (non-static).
  // * Add a new AseSprite (wrapper of a generic Sprite).
  // * Add ASE delegates to listen changes in ASE configuration
  // * AseSprite should implement a delegate to listen changes to the undo limit, 
  // * When a change is produced, AseSprite updates the wrappedSprite->getUndo()->setUndoLimit().
  int undo_size_limit = get_config_int("Options", "UndoSizeLimit", 8)*1024*1024;

  // More differences.
  m_diffCount++;

  // Reset the "redo" stream.
  clearRedo();

  if (out_of_group(m_undoStream)) {
    int groups = count_undo_groups(m_undoStream);

    // "undo" is too big?
    while (groups > 1 && m_undoStream->getMemSize() > undo_size_limit) {
      discardTail();
      groups--;
    }
  }
}

/***********************************************************************

  Raw data

***********************************************************************/

#define read_raw_uint32(dst)		\
  {					\
    memcpy(&dword, raw_data, 4);	\
    dst = dword;			\
    raw_data += 4;			\
  }

#define read_raw_uint16(dst)		\
  {					\
    memcpy(&word, raw_data, 2);		\
    dst = word;				\
    raw_data += 2;			\
  }

#define read_raw_int16(dst)		\
  {					\
    memcpy(&word, raw_data, 2);		\
    dst = (int16_t)word;		\
    raw_data += 2;			\
  }

#define read_raw_uint8(dst)		\
  {					\
    dst = *raw_data;			\
    ++raw_data;				\
  }

#define read_raw_data(dst, size)	\
  {					\
    memcpy(dst, raw_data, size);	\
    raw_data += size;			\
  }

#define write_raw_uint32(src)		\
  {					\
    dword = src;			\
    memcpy(raw_data, &dword, 4);	\
    raw_data += 4;			\
  }

#define write_raw_uint16(src)		\
  {					\
    word = src;				\
    memcpy(raw_data, &word, 2);		\
    raw_data += 2;			\
  }

#define write_raw_int16(src)		\
  {					\
    word = (int16_t)src;		\
    memcpy(raw_data, &word, 2);		\
    raw_data += 2;			\
  }

#define write_raw_uint8(src)		\
  {					\
    *raw_data = src;			\
    ++raw_data;				\
  }

#define write_raw_data(src, size)	\
  {					\
    memcpy(raw_data, src, size);	\
    raw_data += size;			\
  }

/***********************************************************************

  "open"

     no data

***********************************************************************/

void UndoHistory::undo_open()
{
  chunk_open_new(m_undoStream);
  updateUndo();
}

static void chunk_open_new(UndoStream* stream)
{
  undo_chunk_new(stream,
		 UNDO_TYPE_OPEN,
		 sizeof(UndoChunk));
}

static void chunk_open_invert(UndoStream* stream, UndoChunk* chunk)
{
  chunk_close_new(stream);
}

/***********************************************************************

  "close"

     no data

***********************************************************************/

void UndoHistory::undo_close()
{
  chunk_close_new(m_undoStream);
  updateUndo();
}

static void chunk_close_new(UndoStream* stream)
{
  undo_chunk_new(stream,
		 UNDO_TYPE_CLOSE,
		 sizeof(UndoChunk));
}

static void chunk_close_invert(UndoStream* stream, UndoChunk* chunk)
{
  chunk_open_new(stream);
}

/***********************************************************************

  "data"

     DWORD		object ID
     DWORD		field address offset
     DWORD		field data size
     BYTE[]		field data bytes

***********************************************************************/

struct UndoChunkData
{
  UndoChunk head;
  ObjectId objectId;
  uint32_t fieldOffset;
  uint32_t fieldSize;
  uint8_t fieldData[0];
};

void UndoHistory::undo_data(void* object, void* fieldAddress, int fieldSize)
{
  chunk_data_new(m_undoStream, object, fieldAddress, fieldSize);
  updateUndo();
}

static void chunk_data_new(UndoStream* stream, void* object, void* fieldAddress, int fieldSize)
{
  uint32_t fieldOffset = (uint32_t)(((uint8_t*)fieldAddress) -
				    ((uint8_t*)object));

  ASSERT(fieldSize >= 1);

  UndoChunkData* chunk = (UndoChunkData *)
    undo_chunk_new(stream,
		   UNDO_TYPE_DATA,
		   sizeof(UndoChunkData) + fieldSize);

  chunk->objectId = stream->getObjects()->addObject(object);
  chunk->fieldOffset = fieldOffset;
  chunk->fieldSize = fieldSize;

  memcpy(chunk->fieldData, fieldAddress, fieldSize);
}

static void chunk_data_invert(UndoStream* stream, UndoChunkData *chunk)
{
  unsigned int offset = chunk->fieldOffset;
  unsigned int size = chunk->fieldSize;
  void* object = stream->getObjects()->getObject(chunk->objectId);
  void* field = (void*)(((uint8_t*)object) + offset);

  // Save the current data
  chunk_data_new(stream, object, field, size);

  // Copy back the old data
  memcpy(field, chunk->fieldData, size);
}

/***********************************************************************

  "image"

     DWORD		image ID
     BYTE		image type
     WORD[4]		x, y, w, h
     for each row ("h" times)
       for each pixel ("w" times)
	  BYTE[4]	for RGB images, or
	  BYTE[2]	for Grayscale images, or
	  BYTE		for Indexed images

***********************************************************************/

struct UndoChunkImage
{
  UndoChunk head;
  ObjectId image_id;
  uint8_t imgtype;
  uint16_t x, y, w, h; 
  uint8_t data[0];
};

void UndoHistory::undo_image(Image* image, int x, int y, int w, int h)
{
  chunk_image_new(m_undoStream, image, x, y, w, h);
  updateUndo();
}

static void chunk_image_new(UndoStream* stream, Image* image, int x, int y, int w, int h)
{
  UndoChunkImage* chunk;
  uint8_t* ptr;
  int v, size;

  ASSERT(w >= 1 && h >= 1);
  ASSERT(x >= 0 && y >= 0 && x+w <= image->w && y+h <= image->h);
  
  size = image_line_size(image, w);

  chunk = (UndoChunkImage*)
    undo_chunk_new(stream,
		   UNDO_TYPE_IMAGE,
		   sizeof(UndoChunkImage) + size*h);
  
  chunk->image_id = stream->getObjects()->addObject(image);
  chunk->imgtype = image->imgtype;
  chunk->x = x;
  chunk->y = y;
  chunk->w = w;
  chunk->h = h;

  ptr = chunk->data;
  for (v=0; v<h; ++v) {
    memcpy(ptr, image_address(image, x, y+v), size);
    ptr += size;
  }
}

static void chunk_image_invert(UndoStream* stream, UndoChunkImage* chunk)
{
  ObjectId id = chunk->image_id;
  int imgtype = chunk->imgtype;
  Image* image = stream->getObjects()->getObjectT<Image>(id);

  if (image->imgtype != imgtype)
    throw UndoException("Image type does not match");

  int x, y, w, h;
  uint8_t* ptr;
  int v, size;

  x = chunk->x;
  y = chunk->y;
  w = chunk->w;
  h = chunk->h;

  // Backup the current image portion
  chunk_image_new(stream, image, x, y, w, h);

  // Restore the old image portion
  size = image_line_size(image, chunk->w);
  ptr = chunk->data;

  for (v=0; v<h; ++v) {
    memcpy(image_address(image, x, y+v), ptr, size);
    ptr += size;
  }
}

/***********************************************************************

  "flip"

     DWORD		image ID
     BYTE		image type
     WORD[4]		x1, y1, x2, y2
     BYTE		1=horizontal 0=vertical

***********************************************************************/

struct UndoChunkFlip
{
  UndoChunk head;
  ObjectId image_id;
  uint8_t imgtype;
  uint16_t x1, y1, x2, y2; 
  uint8_t horz;
};

void UndoHistory::undo_flip(Image* image, int x1, int y1, int x2, int y2, bool horz)
{
  chunk_flip_new(m_undoStream, image, x1, y1, x2, y2, horz);
  updateUndo();
}

static void chunk_flip_new(UndoStream* stream, Image* image, int x1, int y1, int x2, int y2, bool horz)
{
  UndoChunkFlip *chunk = (UndoChunkFlip *)
    undo_chunk_new(stream,
		   UNDO_TYPE_FLIP,
		   sizeof(UndoChunkFlip));

  chunk->image_id = stream->getObjects()->addObject(image);
  chunk->imgtype = image->imgtype;
  chunk->x1 = x1;
  chunk->y1 = y1;
  chunk->x2 = x2;
  chunk->y2 = y2;
  chunk->horz = horz ? 1: 0;
}

static void chunk_flip_invert(UndoStream* stream, UndoChunkFlip* chunk)
{
  Image* image = stream->getObjects()->getObjectT<Image>(chunk->image_id);

  if ((image) &&
      (image->getType() == GFXOBJ_IMAGE) &&
      (image->imgtype == chunk->imgtype)) {
    int x1 = chunk->x1;
    int y1 = chunk->y1;
    int x2 = chunk->x2;
    int y2 = chunk->y2;
    bool horz = (chunk->horz != 0);

    chunk_flip_new(stream, image, x1, y1, x2, y2, horz);

    Image* area = image_crop(image, x1, y1, x2-x1+1, y2-y1+1, 0);
    int x, y;

    for (y=0; y<(y2-y1+1); y++)
      for (x=0; x<(x2-x1+1); x++)
	image_putpixel(image,
		       horz ? x2-x: x1+x,
		       !horz? y2-y: y1+y,
		       image_getpixel(area, x, y));

    image_free(area);
  }
}

/***********************************************************************

  "dirty"

     DWORD		image ID
     DIRTY_DATA		see read/write_raw_dirty

***********************************************************************/

struct UndoChunkDirty
{
  UndoChunk head;
  ObjectId image_id;
  uint8_t data[0];
};

void UndoHistory::undo_dirty(Image* image, Dirty* dirty)
{
  chunk_dirty_new(m_undoStream, image, dirty);
  updateUndo();
}

static void chunk_dirty_new(UndoStream* stream, Image* image, Dirty *dirty)
{
  UndoChunkDirty *chunk = (UndoChunkDirty *)
    undo_chunk_new(stream,
		   UNDO_TYPE_DIRTY,
		   sizeof(UndoChunkDirty)+get_raw_dirty_size(dirty));

  chunk->image_id = stream->getObjects()->addObject(image);
  write_raw_dirty(chunk->data, dirty);
}

static void chunk_dirty_invert(UndoStream* stream, UndoChunkDirty *chunk)
{
  Image* image = stream->getObjects()->getObjectT<Image>(chunk->image_id);

  if ((image) &&
      (image->getType() == GFXOBJ_IMAGE)) {
    Dirty* dirty = read_raw_dirty(chunk->data);

    if (dirty != NULL) {
      dirty->swapImagePixels(image);
      chunk_dirty_new(stream, image, dirty);
      delete dirty;
    }
  }
}

/***********************************************************************

  "add_image"

     DWORD		stock ID
     DWORD		index of the image in the stock

***********************************************************************/

struct UndoChunkAddImage
{
  UndoChunk head;
  ObjectId stock_id;
  uint32_t image_index;
};

void UndoHistory::undo_add_image(Stock *stock, int image_index)
{
  chunk_add_image_new(m_undoStream, stock, image_index);
  updateUndo();
}

static void chunk_add_image_new(UndoStream* stream, Stock *stock, int image_index)
{
  UndoChunkAddImage* chunk = (UndoChunkAddImage* )
    undo_chunk_new(stream,
		   UNDO_TYPE_ADD_IMAGE,
		   sizeof(UndoChunkAddImage));

  chunk->stock_id = stream->getObjects()->addObject(stock);
  chunk->image_index = image_index;
}

static void chunk_add_image_invert(UndoStream* stream, UndoChunkAddImage* chunk)
{
  ObjectId stock_id = chunk->stock_id;
  unsigned int image_index = chunk->image_index;
  Stock *stock = stream->getObjects()->getObjectT<Stock>(stock_id);

  if (stock) {
    Image* image = stock->getImage(image_index);
    if (image != NULL) {
      chunk_remove_image_new(stream, stock, image_index);
      stock->removeImage(image);
      image_free(image);
    }
  }
}

/***********************************************************************

  "remove_image"

     DWORD		stock ID
     DWORD		index of the image in the stock
     IMAGE_DATA		see read/write_raw_image

***********************************************************************/

struct UndoChunkRemoveImage
{
  UndoChunk head;
  ObjectId stock_id;
  uint32_t image_index;
  uint8_t data[0];
};

void UndoHistory::undo_remove_image(Stock *stock, int image_index)
{
  chunk_remove_image_new(m_undoStream, stock, image_index);
  updateUndo();
}

static void chunk_remove_image_new(UndoStream* stream, Stock *stock, int image_index)
{
  Image* image = stock->getImage(image_index);
  UndoChunkRemoveImage* chunk = (UndoChunkRemoveImage*)
    undo_chunk_new(stream,
		   UNDO_TYPE_REMOVE_IMAGE,
		   sizeof(UndoChunkRemoveImage)+get_raw_image_size(image));

  chunk->stock_id = stream->getObjects()->addObject(stock);
  chunk->image_index = image_index;

  write_raw_image(stream->getObjects(), chunk->data, image);
}

static void chunk_remove_image_invert(UndoStream* stream, UndoChunkRemoveImage* chunk)
{
  ObjectId stock_id = chunk->stock_id;
  unsigned int image_index = chunk->image_index;
  Stock* stock = stream->getObjects()->getObjectT<Stock>(stock_id);

  if (stock) {
    Image* image = read_raw_image(stream->getObjects(), chunk->data);

    /* ASSERT(image != NULL); */

    stock->replaceImage(image_index, image);
    chunk_add_image_new(stream, stock, image_index);
  }
}

/***********************************************************************

  "replace_image"

     DWORD		stock ID
     DWORD		index of the image in the stock
     IMAGE_DATA		see read/write_raw_image

***********************************************************************/

struct UndoChunkReplaceImage
{
  UndoChunk head;
  ObjectId stock_id;
  uint32_t image_index;
  uint8_t data[0];
};

void UndoHistory::undo_replace_image(Stock *stock, int image_index)
{
  chunk_replace_image_new(m_undoStream, stock, image_index);
  updateUndo();
}

static void chunk_replace_image_new(UndoStream* stream, Stock *stock, int image_index)
{
  Image* image = stock->getImage(image_index);
  UndoChunkReplaceImage* chunk = (UndoChunkReplaceImage* )
    undo_chunk_new(stream,
		   UNDO_TYPE_REPLACE_IMAGE,
		   sizeof(UndoChunkReplaceImage)+get_raw_image_size(image));

  chunk->stock_id = stream->getObjects()->addObject(stock);
  chunk->image_index = image_index;

  write_raw_image(stream->getObjects(), chunk->data, image);
}

static void chunk_replace_image_invert(UndoStream* stream, UndoChunkReplaceImage* chunk)
{
  ObjectId stock_id = chunk->stock_id;
  unsigned long image_index = chunk->image_index;
  Stock* stock = stream->getObjects()->getObjectT<Stock>(stock_id);

  if (stock) {
    // read the image to be restored from the chunk
    Image* image = read_raw_image(stream->getObjects(), chunk->data);

    // save the current image in the (redo) stream
    chunk_replace_image_new(stream, stock, image_index);
    Image* old_image = stock->getImage(image_index);

    // replace the image in the stock
    stock->replaceImage(image_index, image);

    // destroy the old image
    image_free(old_image);
  }
}

/***********************************************************************

  "add_cel"

     DWORD		layer ID
     DWORD		cel ID

***********************************************************************/

struct UndoChunkAddCel
{
  UndoChunk head;
  ObjectId layer_id;
  ObjectId cel_id;
};

void UndoHistory::undo_add_cel(Layer* layer, Cel* cel)
{
  chunk_add_cel_new(m_undoStream, layer, cel);
  updateUndo();
}

static void chunk_add_cel_new(UndoStream* stream, Layer* layer, Cel* cel)
{
  UndoChunkAddCel* chunk = (UndoChunkAddCel* )
    undo_chunk_new(stream,
		   UNDO_TYPE_ADD_CEL,
		   sizeof(UndoChunkAddCel));

  chunk->layer_id = stream->getObjects()->addObject(layer);
  chunk->cel_id = stream->getObjects()->addObject(cel);
}

static void chunk_add_cel_invert(UndoStream* stream, UndoChunkAddCel* chunk)
{
  LayerImage* layer = stream->getObjects()->getObjectT<LayerImage>(chunk->layer_id);
  Cel* cel = stream->getObjects()->getObjectT<Cel>(chunk->cel_id);

  chunk_remove_cel_new(stream, layer, cel);

  layer->removeCel(cel);
  cel_free(cel);
}

/***********************************************************************

  "remove_cel"

     DWORD		layer ID
     CEL_DATA		see read/write_raw_cel

***********************************************************************/

struct UndoChunkRemoveCel
{
  UndoChunk head;
  ObjectId layer_id;
  uint8_t data[0];
};

void UndoHistory::undo_remove_cel(Layer* layer, Cel* cel)
{
  chunk_remove_cel_new(m_undoStream, layer, cel);
  updateUndo();
}

static void chunk_remove_cel_new(UndoStream* stream, Layer* layer, Cel* cel)
{
  UndoChunkRemoveCel* chunk = (UndoChunkRemoveCel*)
    undo_chunk_new(stream,
		   UNDO_TYPE_REMOVE_CEL,
		   sizeof(UndoChunkRemoveCel)+get_raw_cel_size(cel));

  chunk->layer_id = stream->getObjects()->addObject(layer);
  write_raw_cel(stream->getObjects(), chunk->data, cel);
}

static void chunk_remove_cel_invert(UndoStream* stream, UndoChunkRemoveCel* chunk)
{
  ObjectId layer_id = chunk->layer_id;
  LayerImage* layer = stream->getObjects()->getObjectT<LayerImage>(layer_id);

  // Read the cel
  Cel* cel = read_raw_cel(stream->getObjects(), chunk->data);

  chunk_add_cel_new(stream, layer, cel);
  layer->addCel(cel);
}

/***********************************************************************

  "set_layer_name"

     DWORD		layer ID
     DWORD		name length
     BYTES[length]	name text

***********************************************************************/

struct UndoChunkSetLayerName
{
  UndoChunk head;
  ObjectId layer_id;
  uint16_t name_length;
  uint8_t name_text[0];
};

void UndoHistory::undo_set_layer_name(Layer* layer)
{
  chunk_set_layer_name_new(m_undoStream, layer);
  updateUndo();
}

static void chunk_set_layer_name_new(UndoStream* stream, Layer *layer)
{
  std::string layer_name = layer->getName();

  UndoChunkSetLayerName* chunk = (UndoChunkSetLayerName*)
    undo_chunk_new(stream,
  		   UNDO_TYPE_SET_LAYER_NAME,
  		   sizeof(UndoChunkSetLayerName) + layer_name.size());

  chunk->layer_id = stream->getObjects()->addObject(layer);
  chunk->name_length = layer_name.size();

  for (int c=0; c<chunk->name_length; c++)
    chunk->name_text[c] = layer_name[c];
}

static void chunk_set_layer_name_invert(UndoStream* stream, UndoChunkSetLayerName* chunk)
{
  Layer* layer = stream->getObjects()->getObjectT<Layer>(chunk->layer_id);

  if (layer) {
    chunk_set_layer_name_new(stream, layer);

    std::string layer_name;
    layer_name.reserve(chunk->name_length);

    for (int c=0; c<chunk->name_length; c++)
      layer_name.push_back(chunk->name_text[c]);

    layer->setName(layer_name.c_str());
  }
}

/***********************************************************************

  "add_layer"

     DWORD		parent layer set ID
     DWORD		layer ID

***********************************************************************/

struct UndoChunkAddLayer
{
  UndoChunk head;
  ObjectId folder_id;
  ObjectId layer_id;
};

void UndoHistory::undo_add_layer(Layer* folder, Layer* layer)
{
  chunk_add_layer_new(m_undoStream, folder, layer);
  updateUndo();
}

static void chunk_add_layer_new(UndoStream* stream, Layer* folder, Layer* layer)
{
  UndoChunkAddLayer* chunk = (UndoChunkAddLayer* )
    undo_chunk_new(stream,
		   UNDO_TYPE_ADD_LAYER,
		   sizeof(UndoChunkAddLayer));

  chunk->folder_id = stream->getObjects()->addObject(folder);
  chunk->layer_id = stream->getObjects()->addObject(layer);
}

static void chunk_add_layer_invert(UndoStream* stream, UndoChunkAddLayer* chunk)
{
  LayerFolder* folder = stream->getObjects()->getObjectT<LayerFolder>(chunk->folder_id);
  Layer* layer = stream->getObjects()->getObjectT<Layer>(chunk->layer_id);

  chunk_remove_layer_new(stream, layer);

  folder->remove_layer(layer);
  delete layer;
}

/***********************************************************************

  "remove_layer"

     DWORD		parent layer folder ID
     DWORD		after layer ID
     LAYER_DATA		see read/write_raw_layer

***********************************************************************/

struct UndoChunkRemoveLayer
{
  UndoChunk head;
  ObjectId folder_id;
  ObjectId after_id;
  uint8_t data[0];
};

void UndoHistory::undo_remove_layer(Layer* layer)
{
  chunk_remove_layer_new(m_undoStream, layer);
  updateUndo();
}

static void chunk_remove_layer_new(UndoStream* stream, Layer* layer)
{
  UndoChunkRemoveLayer* chunk = (UndoChunkRemoveLayer*)
    undo_chunk_new(stream,
		   UNDO_TYPE_REMOVE_LAYER,
		   sizeof(UndoChunkRemoveLayer)+get_raw_layer_size(layer));
  LayerFolder* folder = layer->get_parent();
  Layer* after = layer->get_prev();

  chunk->folder_id = stream->getObjects()->addObject(folder);
  chunk->after_id = (after != NULL ? stream->getObjects()->addObject(after): 0);

  write_raw_layer(stream->getObjects(), chunk->data, layer);
}

static void chunk_remove_layer_invert(UndoStream* stream, UndoChunkRemoveLayer* chunk)
{
  LayerFolder* folder = stream->getObjects()->getObjectT<LayerFolder>(chunk->folder_id);
  Layer* layer = read_raw_layer(stream->getObjects(), chunk->data);
  Layer* after = (chunk->after_id != 0 ? stream->getObjects()->getObjectT<Layer>(chunk->after_id): NULL);

  chunk_add_layer_new(stream, folder, layer);

  folder->add_layer(layer);
  folder->move_layer(layer, after);
}

/***********************************************************************

  "move_layer"

     DWORD		parent layer folder ID
     DWORD		layer ID
     DWORD		after layer ID

***********************************************************************/

struct UndoChunkMoveLayer
{
  UndoChunk head;
  ObjectId folder_id;
  ObjectId layer_id;
  ObjectId after_id;
};

void UndoHistory::undo_move_layer(Layer* layer)
{
  chunk_move_layer_new(m_undoStream, layer);
  updateUndo();
}

static void chunk_move_layer_new(UndoStream* stream, Layer* layer)
{
  UndoChunkMoveLayer* chunk = (UndoChunkMoveLayer* )
    undo_chunk_new(stream,
		   UNDO_TYPE_MOVE_LAYER,
		   sizeof(UndoChunkMoveLayer));
  LayerFolder* folder = layer->get_parent();
  Layer* after = layer->get_prev();

  chunk->folder_id = stream->getObjects()->addObject(folder);
  chunk->layer_id = stream->getObjects()->addObject(layer);
  chunk->after_id = (after ? stream->getObjects()->addObject(after): 0);
}

static void chunk_move_layer_invert(UndoStream* stream, UndoChunkMoveLayer* chunk)
{
  LayerFolder* folder = stream->getObjects()->getObjectT<LayerFolder>(chunk->folder_id);
  Layer* layer = stream->getObjects()->getObjectT<Layer>(chunk->layer_id);
  Layer* after = (chunk->after_id != 0 ? stream->getObjects()->getObjectT<Layer>(chunk->after_id): NULL);

  chunk_move_layer_new(stream, layer);
  folder->move_layer(layer, after);
}

/***********************************************************************

  "set_layer"

     DWORD		sprite ID
     DWORD		layer ID

***********************************************************************/

struct UndoChunkSetLayer
{
  UndoChunk head;
  ObjectId sprite_id;
  ObjectId layer_id;
};

void UndoHistory::undo_set_layer(Sprite *sprite)
{
  chunk_set_layer_new(m_undoStream, sprite);
  updateUndo();
}

static void chunk_set_layer_new(UndoStream* stream, Sprite *sprite)
{
  UndoChunkSetLayer* chunk = (UndoChunkSetLayer*)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_LAYER,
		   sizeof(UndoChunkSetLayer));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->layer_id = (sprite->getCurrentLayer() ?
		     stream->getObjects()->addObject(sprite->getCurrentLayer()): 0);
}

static void chunk_set_layer_invert(UndoStream* stream, UndoChunkSetLayer* chunk)
{
  Sprite *sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);
  Layer* layer = (chunk->layer_id != 0 ? stream->getObjects()->getObjectT<Layer>(chunk->layer_id): NULL);

  chunk_set_layer_new(stream, sprite);

  sprite->setCurrentLayer(layer);
}

/***********************************************************************

  "add_palette"

     DWORD		sprite ID
     DWORD		palette ID

***********************************************************************/

struct UndoChunkAddPalette
{
  UndoChunk head;
  ObjectId sprite_id;
  ObjectId palette_id;
};

void UndoHistory::undo_add_palette(Sprite *sprite, Palette* palette)
{
  chunk_add_palette_new(m_undoStream, sprite, palette);
  updateUndo();
}

static void chunk_add_palette_new(UndoStream* stream, Sprite *sprite, Palette* palette)
{
  UndoChunkAddPalette* chunk = (UndoChunkAddPalette*)
    undo_chunk_new(stream,
		   UNDO_TYPE_ADD_PALETTE,
		   sizeof(UndoChunkAddPalette));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->palette_id = stream->getObjects()->addObject(palette);
}

static void chunk_add_palette_invert(UndoStream* stream, UndoChunkAddPalette *chunk)
{
  Sprite* sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);
  Palette* palette = stream->getObjects()->getObjectT<Palette>(chunk->palette_id);

  chunk_remove_palette_new(stream, sprite, palette);
  sprite->deletePalette(palette);
}

/***********************************************************************

  "remove_palette"

     DWORD		sprite ID
     PALETTE_DATA	see read/write_raw_palette

***********************************************************************/

struct UndoChunkRemovePalette
{
  UndoChunk head;
  ObjectId sprite_id;
  uint8_t data[0];
};

void UndoHistory::undo_remove_palette(Sprite *sprite, Palette* palette)
{
  chunk_remove_palette_new(m_undoStream, sprite, palette);
  updateUndo();
}

static void chunk_remove_palette_new(UndoStream* stream, Sprite *sprite, Palette* palette)
{
  UndoChunkRemovePalette* chunk = (UndoChunkRemovePalette*)
    undo_chunk_new(stream,
		   UNDO_TYPE_REMOVE_PALETTE,
		   sizeof(UndoChunkRemovePalette)+get_raw_palette_size(palette));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  write_raw_palette(chunk->data, palette);
}

static void chunk_remove_palette_invert(UndoStream* stream, UndoChunkRemovePalette *chunk)
{
  Sprite *sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);
  Palette* palette = read_raw_palette(chunk->data);

  chunk_add_palette_new(stream, sprite, palette);
  sprite->setPalette(palette, true);

  delete palette;
}

/***********************************************************************

  "set_palette_colors"

     DWORD		sprite ID
     DWORD		frame
     BYTE               from
     BYTE               to
     DWORD[to-from+1]   palette entries

***********************************************************************/

struct UndoChunkSetPaletteColors
{
  UndoChunk head;
  ObjectId sprite_id;
  uint32_t frame;
  uint8_t from;
  uint8_t to;
  uint8_t data[0];
};

void UndoHistory::undo_set_palette_colors(Sprite *sprite, Palette* palette, int from, int to)
{
  chunk_set_palette_colors_new(m_undoStream, sprite, palette, from, to);
  updateUndo();
}

static void chunk_set_palette_colors_new(UndoStream* stream, Sprite *sprite, Palette* palette, int from, int to)
{
  UndoChunkSetPaletteColors* chunk = (UndoChunkSetPaletteColors*)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_PALETTE_COLORS,
		   sizeof(UndoChunkSetPaletteColors) + sizeof(uint32_t)*(to-from+1));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->frame = sprite->getCurrentFrame();
  chunk->from = from;
  chunk->to = to;

  // Write (to-from+1) palette color entries
  uint32_t dword;
  uint8_t* raw_data = chunk->data;
  for (int i=from; i<=to; ++i)
    write_raw_uint32(palette->getEntry(i));
}

static void chunk_set_palette_colors_invert(UndoStream* stream, UndoChunkSetPaletteColors *chunk)
{
  Sprite* sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);
  Palette* palette = sprite->getPalette(chunk->frame);
  if (palette == NULL)
    throw UndoException("chunk_set_palette_colors_invert: palette not found");

  // Add the chunk to invert the operation
  chunk_set_palette_colors_new(stream, sprite, palette, chunk->from, chunk->to);

  uint32_t dword;
  uint32_t color;
  uint8_t* raw_data = chunk->data;

  for (int i=(int)chunk->from; i<=(int)chunk->to; ++i) {
    read_raw_uint32(color);
    palette->setEntry(i, color);
  }
}

/***********************************************************************

  "remap_palette"

     DWORD		sprite ID
     DWORD		first frame in range
     DWORD		last frame in range
     BYTE[256]		mapping table

***********************************************************************/

struct UndoChunkRemapPalette
{
  UndoChunk head;
  ObjectId sprite_id;
  uint32_t frame_from;
  uint32_t frame_to;
  uint8_t mapping[256];
};

void UndoHistory::undo_remap_palette(Sprite* sprite, int frame_from, int frame_to, const std::vector<int>& mapping)
{
  chunk_remap_palette_new(m_undoStream, sprite, frame_from, frame_to, mapping);
  updateUndo();
}

static void chunk_remap_palette_new(UndoStream* stream, Sprite *sprite, int frame_from, int frame_to, const std::vector<int>& mapping)
{
  UndoChunkRemapPalette* chunk = (UndoChunkRemapPalette*)
    undo_chunk_new(stream,
		   UNDO_TYPE_REMAP_PALETTE,
		   sizeof(UndoChunkRemapPalette));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->frame_from = frame_from;
  chunk->frame_to = frame_to;

  ASSERT(mapping.size() == 256 && "Mapping tables must have 256 entries");

  for (size_t c=0; c<256; c++)
    chunk->mapping[c] = mapping[c];
}

static void chunk_remap_palette_invert(UndoStream* stream, UndoChunkRemapPalette* chunk)
{
  Sprite *sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);

  // Inverse mapping
  std::vector<int> inverse_mapping(256);
  for (size_t c=0; c<256; ++c)
    inverse_mapping[chunk->mapping[c]] = c;

  chunk_remap_palette_new(stream, sprite, chunk->frame_from, chunk->frame_to, inverse_mapping);

  // Remap in inverse order
  sprite->remapImages(chunk->frame_from, chunk->frame_to, inverse_mapping);
}

/***********************************************************************

  "set_mask"

     DWORD		document ID
     MASK_DATA		see read/write_raw_mask

***********************************************************************/

struct UndoChunkSetMask
{
  UndoChunk head;
  ObjectId doc_id;
  uint8_t data[0];
};

void UndoHistory::undo_set_mask(Document* document)
{
  chunk_set_mask_new(m_undoStream, document);
  updateUndo();
}

static void chunk_set_mask_new(UndoStream* stream, Document* document)
{
  UndoChunkSetMask* chunk = (UndoChunkSetMask*)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_MASK,
		   sizeof(UndoChunkSetMask)+get_raw_mask_size(document->getMask()));

  chunk->doc_id = stream->getObjects()->addObject(document);
  write_raw_mask(chunk->data, document->getMask());
}

static void chunk_set_mask_invert(UndoStream* stream, UndoChunkSetMask* chunk)
{
  Document* document = stream->getObjects()->getObjectT<Document>(chunk->doc_id);
  ASSERT(document != NULL);

  if (document != NULL) {
    Mask* mask = read_raw_mask(chunk->data);

    chunk_set_mask_new(stream, document);
    mask_copy(document->getMask(), mask);
    mask_free(mask);
  }
}

/***********************************************************************

  "set_imgtype"

     DWORD		sprite ID
     DWORD		imgtype

***********************************************************************/

struct UndoChunkSetImgType
{
  UndoChunk head;
  ObjectId sprite_id;
  uint32_t imgtype;
};

void UndoHistory::undo_set_imgtype(Sprite* sprite)
{
  chunk_set_imgtype_new(m_undoStream, sprite);
  updateUndo();
}

static void chunk_set_imgtype_new(UndoStream* stream, Sprite* sprite)
{
  UndoChunkSetImgType* chunk = (UndoChunkSetImgType*)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_IMGTYPE,
		   sizeof(UndoChunkSetImgType));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->imgtype = sprite->getImgType();
}

static void chunk_set_imgtype_invert(UndoStream* stream, UndoChunkSetImgType* chunk)
{
  Sprite* sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);

  if (sprite) {
    chunk_set_imgtype_new(stream, sprite);
    sprite->setImgType(chunk->imgtype);
  }
}

/***********************************************************************

  "set_size"

     DWORD		sprite ID
     DWORD		width
     DWORD		height

***********************************************************************/

struct UndoChunkSetSize
{
  UndoChunk head;
  ObjectId sprite_id;
  uint32_t width;
  uint32_t height;
};

void UndoHistory::undo_set_size(Sprite* sprite)
{
  chunk_set_size_new(m_undoStream, sprite);
  updateUndo();
}

static void chunk_set_size_new(UndoStream* stream, Sprite* sprite)
{
  UndoChunkSetSize* chunk = (UndoChunkSetSize*)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_SIZE,
		   sizeof(UndoChunkSetSize));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->width = sprite->getWidth();
  chunk->height = sprite->getHeight();
}

static void chunk_set_size_invert(UndoStream* stream, UndoChunkSetSize *chunk)
{
  Sprite *sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);

  if (sprite) {
    chunk_set_size_new(stream, sprite);
    sprite->setSize(chunk->width, chunk->height);
  }
}

/***********************************************************************

  "set_frame"

     DWORD		sprite ID
     DWORD		frame

***********************************************************************/

struct UndoChunkSetFrame
{
  UndoChunk head;
  ObjectId sprite_id;
  uint32_t frame;
};

void UndoHistory::undo_set_frame(Sprite* sprite)
{
  chunk_set_frame_new(m_undoStream, sprite);
  updateUndo();
}

static void chunk_set_frame_new(UndoStream* stream, Sprite* sprite)
{
  UndoChunkSetFrame* chunk = (UndoChunkSetFrame*)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_FRAME,
		   sizeof(UndoChunkSetFrame));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->frame = sprite->getCurrentFrame();
}

static void chunk_set_frame_invert(UndoStream* stream, UndoChunkSetFrame *chunk)
{
  Sprite* sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);

  if (sprite) {
    chunk_set_frame_new(stream, sprite);
    sprite->setCurrentFrame(chunk->frame);
  }
}

/***********************************************************************

  "set_frames"

     DWORD		sprite ID
     DWORD		frames

***********************************************************************/

struct UndoChunkSetFrames
{
  UndoChunk head;
  ObjectId sprite_id;
  uint32_t frames;
};

void UndoHistory::undo_set_frames(Sprite *sprite)
{
  chunk_set_frames_new(m_undoStream, sprite);
  updateUndo();
}

static void chunk_set_frames_new(UndoStream* stream, Sprite *sprite)
{
  UndoChunkSetFrames *chunk = (UndoChunkSetFrames *)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_FRAMES,
		   sizeof(UndoChunkSetFrames));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->frames = sprite->getTotalFrames();
}

static void chunk_set_frames_invert(UndoStream* stream, UndoChunkSetFrames *chunk)
{
  Sprite* sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);

  if (sprite) {
    chunk_set_frames_new(stream, sprite);
    sprite->setTotalFrames(chunk->frames);
  }
}

/***********************************************************************

  "set_frlen"

     DWORD		sprite ID
     DWORD		frame
     DWORD		duration

***********************************************************************/

struct UndoChunkSetFrlen
{
  UndoChunk head;
  ObjectId sprite_id;
  uint32_t frame;
  uint32_t duration;
};

void UndoHistory::undo_set_frlen(Sprite *sprite, int frame)
{
  chunk_set_frlen_new(m_undoStream, sprite, frame);
  updateUndo();
}

static void chunk_set_frlen_new(UndoStream* stream, Sprite *sprite, int frame)
{
  ASSERT(frame >= 0 && frame < sprite->getTotalFrames());

  UndoChunkSetFrlen *chunk = (UndoChunkSetFrlen *)
    undo_chunk_new(stream,
		   UNDO_TYPE_SET_FRLEN,
		   sizeof(UndoChunkSetFrlen));

  chunk->sprite_id = stream->getObjects()->addObject(sprite);
  chunk->frame = frame;
  chunk->duration = sprite->getFrameDuration(frame);
}

static void chunk_set_frlen_invert(UndoStream* stream, UndoChunkSetFrlen *chunk)
{
  Sprite* sprite = stream->getObjects()->getObjectT<Sprite>(chunk->sprite_id);

  if (sprite != NULL) {
    chunk_set_frlen_new(stream, sprite, chunk->frame);
    sprite->setFrameDuration(chunk->frame, chunk->duration);
  }
}

/***********************************************************************

  Helper routines for UndoChunk

***********************************************************************/

static UndoChunk* undo_chunk_new(UndoStream* stream, int type, int size)
{
  UndoChunk* chunk;

  ASSERT(size >= (int)sizeof(UndoChunk));

  chunk = (UndoChunk*)base_malloc0(size);
  if (!chunk)
    return NULL;

  chunk->type = type;
  chunk->size = size;
  chunk->label = stream->getUndo()->getLabel() ?
    stream->getUndo()->getLabel():
    undo_actions[chunk->type].name;

  stream->pushChunk(chunk);
  return chunk;
}

static void undo_chunk_free(UndoChunk* chunk)
{
  base_free(chunk);
}

/***********************************************************************

  Raw dirty data

     BYTE		image type
     WORD[4]		x1, y1, x2, y2
     WORD		rows
     for each row
       WORD[2]		y, columns
       for each column
         WORD[2]	x, w
	 for each pixel ("w" times)
	   BYTE[4]	for RGB images, or
	   BYTE[2]	for Grayscale images, or
	   BYTE		for Indexed images

***********************************************************************/

static Dirty *read_raw_dirty(uint8_t* raw_data)
{
  uint16_t word;
  int x1, y1, x2, y2, size;
  int u, v, x, y, w;
  int imgtype;
  Dirty *dirty = NULL;

  read_raw_uint8(imgtype);
  read_raw_uint16(x1);
  read_raw_uint16(y1);
  read_raw_uint16(x2);
  read_raw_uint16(y2);

  dirty = new Dirty(imgtype, x1, y1, x2, y2);

  int noRows = 0;
  read_raw_uint16(noRows);
  if (noRows > 0) {
    dirty->m_rows.resize(noRows);

    for (v=0; v<dirty->getRowsCount(); v++) {
      y = 0;
      read_raw_uint16(y);

      Dirty::Row* row = new Dirty::Row(y);

      int noCols = 0;
      read_raw_uint16(noCols);
      row->cols.resize(noCols);

      for (u=0; u<noCols; u++) {
	read_raw_uint16(x);
	read_raw_uint16(w);

	Dirty::Col* col = new Dirty::Col(x, w);

	size = dirty->getLineSize(col->w);
	ASSERT(size > 0);

	col->data.resize(size);
	read_raw_data(&col->data[0], size);

	row->cols[u] = col;
      }

      dirty->m_rows[v] = row;
    }
  }

  return dirty;
}

static uint8_t* write_raw_dirty(uint8_t* raw_data, Dirty* dirty)
{
  uint16_t word;

  write_raw_uint8(dirty->getImgType());
  write_raw_uint16(dirty->x1());
  write_raw_uint16(dirty->y1());
  write_raw_uint16(dirty->x2());
  write_raw_uint16(dirty->y2());
  write_raw_uint16(dirty->getRowsCount());

  for (int v=0; v<dirty->getRowsCount(); v++) {
    const Dirty::Row& row = dirty->getRow(v);

    write_raw_uint16(row.y);
    write_raw_uint16(row.cols.size());

    for (size_t u=0; u<row.cols.size(); u++) {
      write_raw_uint16(row.cols[u]->x);
      write_raw_uint16(row.cols[u]->w);

      size_t size = dirty->getLineSize(row.cols[u]->w);
      write_raw_data(&row.cols[u]->data[0], size);
    }
  }

  return raw_data;
}

static int get_raw_dirty_size(Dirty* dirty)
{
  int size = 1+2*4+2;	// BYTE+WORD[4]+WORD

  for (int v=0; v<dirty->getRowsCount(); v++) {
    const Dirty::Row& row = dirty->getRow(v);

    size += 4;			// y, cols (WORD[2])
    for (size_t u=0; u<row.cols.size(); u++) {
      size += 4;		// x, w (WORD[2])
      size += dirty->getLineSize(row.cols[u]->w);
    }
  }

  return size;
}

/***********************************************************************

  Raw image data

     DWORD		image ID
     BYTE		image type
     WORD[2]		w, h
     DWORD		mask color
     for each line	("h" times)
       for each pixel ("w" times)
	 BYTE[4]	for RGB images, or
	 BYTE[2]	for Grayscale images, or
	 BYTE		for Indexed images

***********************************************************************/

static Image* read_raw_image(ObjectsContainer* objects, uint8_t* raw_data)
{
  uint32_t dword;
  uint16_t word;
  ObjectId image_id;
  int imgtype;
  int width;
  int height;
  uint32_t mask_color;
  Image* image;
  int c, size;

  read_raw_uint32(image_id);	/* ID */
  if (!image_id)
    return NULL;

  read_raw_uint8(imgtype);	   /* imgtype */
  read_raw_uint16(width);	   /* width */
  read_raw_uint16(height);	   /* height */
  read_raw_uint32(mask_color);	   /* mask color */

  image = image_new(imgtype, width, height);
  size = image_line_size(image, image->w);

  for (c=0; c<image->h; c++)
    read_raw_data(image->line[c], size);

  image->mask_color = mask_color;

  objects->insertObject(image_id, image);
  return image;
}

static uint8_t* write_raw_image(ObjectsContainer* objects, uint8_t* raw_data, Image* image)
{
  ObjectId image_id = objects->addObject(image);
  uint32_t dword;
  uint16_t word;
  int c, size;

  write_raw_uint32(image_id);		   // ID
  write_raw_uint8(image->imgtype);	   // imgtype
  write_raw_uint16(image->w);		   // width
  write_raw_uint16(image->h);		   // height
  write_raw_uint32(image->mask_color);	   // mask color

  size = image_line_size(image, image->w);
  for (c=0; c<image->h; c++)
    write_raw_data(image->line[c], size);

  objects->removeObject(image_id);
  return raw_data;
}

static int get_raw_image_size(Image* image)
{
  ASSERT(image != NULL);
  return 4+1+2+2+4+image_line_size(image, image->w) * image->h;
}

/***********************************************************************

  Raw cel data

     DWORD		cel ID
     WORD		frame
     WORD		image index
     WORD[2]		x, y
     WORD		opacity

***********************************************************************/

static Cel* read_raw_cel(ObjectsContainer* objects, uint8_t* raw_data)
{
  uint32_t dword;
  uint16_t word;
  int frame, image, x, y, opacity;
  ObjectId cel_id;
  Cel* cel;

  read_raw_uint32(cel_id);
  read_raw_uint16(frame);
  read_raw_uint16(image);
  read_raw_int16(x);
  read_raw_int16(y);
  read_raw_uint16(opacity);

  cel = cel_new(frame, image);
  cel_set_position(cel, x, y);
  cel_set_opacity(cel, opacity);
  
  objects->insertObject(cel_id, cel);
  return cel;
}

static uint8_t* write_raw_cel(ObjectsContainer* objects, uint8_t* raw_data, Cel* cel)
{
  ObjectId cel_id = objects->addObject(cel);
  uint32_t dword;
  uint16_t word;

  write_raw_uint32(cel_id);
  write_raw_uint16(cel->frame);
  write_raw_uint16(cel->image);
  write_raw_int16(cel->x);
  write_raw_int16(cel->y);
  write_raw_uint16(cel->opacity);

  objects->removeObject(cel_id);
  return raw_data;
}

static int get_raw_cel_size(Cel* cel)
{
  return 4+2*5;
}

/***********************************************************************

  Raw layer data

***********************************************************************/

static Layer* read_raw_layer(ObjectsContainer* objects, uint8_t* raw_data)
{
  uint32_t dword;
  uint16_t word;
  ObjectId layer_id, sprite_id;
  std::vector<char> name(1);
  int name_length, flags, layer_type;
  Layer* layer = NULL;
  Sprite *sprite;

  read_raw_uint32(layer_id);			    // ID

  read_raw_uint16(name_length);			    // name length
  name.resize(name_length+1);
  if (name_length > 0) {
    read_raw_data(&name[0], name_length);	    // name
    name[name_length] = 0;
  }
  else
    name[0] = 0;

  read_raw_uint8(flags);			    // flags
  read_raw_uint16(layer_type);			    // type
  read_raw_uint32(sprite_id);			    // sprite

  sprite = objects->getObjectT<Sprite>(sprite_id);

  switch (layer_type) {

    case GFXOBJ_LAYER_IMAGE: {
      int c, cels;

      read_raw_uint16(cels);	  /* cels */

      /* create layer */
      layer = new LayerImage(sprite);

      /* read cels */
      for (c=0; c<cels; c++) {
	Cel* cel;
	uint8_t has_image;

	// Read the cel
	cel = read_raw_cel(objects, raw_data);
	raw_data += get_raw_cel_size(cel);

	/* add the cel in the layer */
	static_cast<LayerImage*>(layer)->addCel(cel);

	/* read the image */
	read_raw_uint8(has_image);
	if (has_image != 0) {
	  Image* image = read_raw_image(objects, raw_data);
	  raw_data += get_raw_image_size(image);

	  layer->getSprite()->getStock()->replaceImage(cel->image, image);
	}
      }
      break;
    }

    case GFXOBJ_LAYER_FOLDER: {
      int c, layers;

      /* create the layer set */
      layer = new LayerFolder(sprite);

      /* read how many sub-layers */
      read_raw_uint16(layers);

      for (c=0; c<layers; c++) {
	Layer* child = read_raw_layer(objects, raw_data);
	if (child) {
	  static_cast<LayerFolder*>(layer)->add_layer(child);
	  raw_data += get_raw_layer_size(child);
	}
	else
	  break;
      }
      break;
    }
      
  }

  if (layer != NULL) {
    layer->setName(&name[0]);
    *layer->flags_addr() = flags;

    objects->insertObject(layer_id, layer);
  }

  return layer;
}

static uint8_t* write_raw_layer(ObjectsContainer* objects, uint8_t* raw_data, Layer* layer)
{
  ObjectId layer_id = objects->addObject(layer);
  uint32_t dword;
  uint16_t word;
  std::string name = layer->getName();

  write_raw_uint32(layer_id);				    // ID

  write_raw_uint16(name.size());			    // Name length
  if (!name.empty())
    write_raw_data(name.c_str(), name.size());		    // Name

  write_raw_uint8(*layer->flags_addr());		    // Flags
  write_raw_uint16(layer->getType());			    // Type
  write_raw_uint32(objects->addObject(layer->getSprite())); // Sprite

  switch (layer->getType()) {

    case GFXOBJ_LAYER_IMAGE: {
      // Cels
      write_raw_uint16(static_cast<LayerImage*>(layer)->getCelsCount());

      CelIterator it = static_cast<LayerImage*>(layer)->getCelBegin();
      CelIterator end = static_cast<LayerImage*>(layer)->getCelEnd();

      for (; it != end; ++it) {
	Cel* cel = *it;
	raw_data = write_raw_cel(objects, raw_data, cel);

	Image* image = layer->getSprite()->getStock()->getImage(cel->image);
	ASSERT(image != NULL);

	write_raw_uint8(1);
	raw_data = write_raw_image(objects, raw_data, image);
      }
      break;
    }

    case GFXOBJ_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->get_layer_begin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->get_layer_end();

      // how many sub-layers
      write_raw_uint16(static_cast<LayerFolder*>(layer)->get_layers_count());

      for (; it != end; ++it)
	raw_data = write_raw_layer(objects, raw_data, *it);
      break;
    }

  }

  objects->removeObject(layer_id);
  return raw_data;
}

static int get_raw_layer_size(Layer* layer)
{
  int size = 4+2+layer->getName().size()+1+2+4;

  switch (layer->getType()) {

    case GFXOBJ_LAYER_IMAGE: {
      size += 1;		// blend mode
      size += 2;		// num of cels

      CelIterator it = static_cast<LayerImage*>(layer)->getCelBegin();
      CelIterator end = static_cast<LayerImage*>(layer)->getCelEnd();

      for (; it != end; ++it) {
	Cel* cel = *it;
	size += get_raw_cel_size(cel);
	size++;			// has image?

	Image* image = layer->getSprite()->getStock()->getImage(cel->image);
	size += get_raw_image_size(image);
      }
      break;
    }

    case GFXOBJ_LAYER_FOLDER: {
      size += 2;		// how many sub-layers

      LayerIterator it = static_cast<LayerFolder*>(layer)->get_layer_begin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->get_layer_end();

      for (; it != end; ++it)
	size += get_raw_layer_size(*it);
      break;
    }

  }

  return size;
}

/***********************************************************************

  Raw palette data

      WORD		frame
      WORD		ncolors
      for each color	("ncolors" times)
        DWORD		_rgba color

***********************************************************************/

static Palette* read_raw_palette(uint8_t* raw_data)
{
  uint32_t dword;
  uint16_t word;
  uint32_t color;
  int frame, ncolors;
  Palette* palette;

  read_raw_uint16(frame);	/* frame */
  read_raw_uint16(ncolors);	/* ncolors */

  palette = new Palette(frame, ncolors);
  if (!palette)
    return NULL;

  for (int c=0; c<ncolors; c++) {
    read_raw_uint32(color);
    palette->setEntry(c, color);
  }

  return palette;
}

static uint8_t* write_raw_palette(uint8_t* raw_data, Palette* palette)
{
  uint32_t dword;
  uint16_t word;
  uint32_t color;

  write_raw_uint16(palette->getFrame()); // frame
  write_raw_uint16(palette->size());	 // number of colors

  for (int c=0; c<palette->size(); c++) {
    color = palette->getEntry(c);
    write_raw_uint32(color);
  }

  return raw_data;
}

static int get_raw_palette_size(Palette* palette)
{
  // 2 WORD + 4 BYTES*ncolors
  return 2*2 + 4*palette->size();
}


/***********************************************************************

  Raw mask data

      WORD[4]		x, y, w, h
      for each line	("h" times)
        for each packet	("((w+7)/8)" times)
          BYTE		8 pixels of the mask

***********************************************************************/

static Mask* read_raw_mask(uint8_t* raw_data)
{
  uint16_t word;
  int x, y, w, h;
  int c, size;
  Mask* mask;

  read_raw_uint16(x);		/* xpos */
  read_raw_uint16(y);		/* ypos */
  read_raw_uint16(w);		/* width */
  read_raw_uint16(h);		/* height */

  mask = mask_new();
  if (!mask)
    return NULL;

  if (w > 0 && h > 0) {
    size = (w+7)/8;

    mask->add(x, y, w, h);
    for (c=0; c<mask->h; c++)
      read_raw_data(mask->bitmap->line[c], size);
  }

  return mask;
}

static uint8_t* write_raw_mask(uint8_t* raw_data, Mask* mask)
{
  uint16_t word;
  int c, size = (mask->w+7)/8;

  write_raw_uint16(mask->x);	/* xpos */
  write_raw_uint16(mask->y);	/* ypos */
  write_raw_uint16(mask->bitmap ? mask->w: 0); /* width */
  write_raw_uint16(mask->bitmap ? mask->h: 0); /* height */

  if (mask->bitmap)
    for (c=0; c<mask->h; c++)
      write_raw_data(mask->bitmap->line[c], size);

  return raw_data;
}

static int get_raw_mask_size(Mask* mask)
{
  int size = (mask->w+7)/8;

  return 2*4 + (mask->bitmap ? mask->h*size: 0);
}