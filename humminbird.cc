/*
    Copyright (C) 2008  Björn Augustsson, oggust@gmail.com
    Copyright (C) 2008  Olaf Klein, o.b.klein@gpsbabel.org
    Copyright (C) 2005-2013 Robert Lipe, robertlipe+source@gpsbabel.org

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

 */

#include "humminbird.h"

#include <QHash>                // for QHash
#include <Qt>                   // for CaseInsensitive
#include <QtGlobal>             // for qRound

#include <cmath>                // for atan, tan, log, sinh
#include <cstdio>               // for SEEK_SET
#include <cstring>              // for strncpy
#include <numbers>              // for inv_pi, pi

#include "defs.h"               // for Waypoint, be_read32, be_read16, be_write32, fatal, be_write16, route_head, track_add_wpt
#include "mkshort.h"            // for MakeShort
#include "src/core/datetime.h"  // for DateTime


#define MYNAME "humminbird"

#define WPT_NAME_LEN		12
#define RTE_NAME_LEN		20
#define TRK_NAME_LEN		20
#define MAX_RTE_POINTS		50
#define MAX_ITEMS_PER_GROUP	12

/*
I suspect that these are actually
struct signature {
	uint8_t format, // 1 = track, 2 = waypoint, 3 = route, 4 = iTrack
	uint8_t version,
	gpuint16 record_length
}

The v3 TRK_MAGIC doesn't have a length, probably because it wouldn't fit.
(It would be 0x200008)

Still, they're useful in the code as a plain signature.
*/
#define TRK_MAGIC		0x01030000L
#define TRK_MAGIC2		0x01021F70L
#define WPT_MAGIC		0x02020024L
#define WPT_MAGIC2		0x02030024L // New for 2013.  No visible diff?!
#define RTE_MAGIC		0x03030088L

#define BAD_CHARS		"\r\n\t"

/* The hwr data format is records-based, and the records are 36 bytes long. */

struct HumminbirdBase::humminbird_waypt_t {
  /* O.K.: the file can also contain routes with a different magic. */
  /* uint32_t signature; */   /* Just for error checking(?) */
  uint16_t num;          /* Always ascending in the file. */
  uint16_t zero;         /* Always seems to be zero. */
  uint8_t  status;       /* Always seems to be 1. Ends up as <h:status>
	                          in gpx files exported by HumminbirdPC. */
  uint8_t  icon;         /* See below */
  uint16_t depth;        /* Water depth. These are fishfinders. In centimeters */
  uint32_t time;         /* This is a time_t. In UTC */
  int32_t  east;
  int32_t  north;
  char     name[WPT_NAME_LEN];
};

struct HumminbirdBase::humminbird_rte_t {
  /* O.K.: the file can contain also routes with a different magic. */
  /* uint32_t signature; */   /* Just for error checking(?) */
  uint16_t num;
  uint16_t zero;
  uint8_t  status;
  uint8_t  U0;
  uint8_t  U1;
  int8_t   count;
  uint32_t time;
  char     name[RTE_NAME_LEN];
  uint16_t points[MAX_RTE_POINTS];
};

struct HumminbirdBase::humminbird_trk_header_t {      /* 68 bytes, incl signature */
  /* uint32_t signature; */
  uint16_t trk_num;
  uint16_t zero;
  uint16_t num_points;
  uint16_t unknown;	/* Always zero so far. */
  uint32_t time;		/* a time_t, in UTC */

  int32_t start_east;	/* Start of track */
  int32_t start_north;
  int32_t end_east;	/* end of track */
  int32_t end_north;

  int32_t sw_east; 	/* Bounding box, enclosing the track */
  int32_t sw_north;	/* sw is the south-west point */
  int32_t ne_east;	/* ne is the north-east point */
  int32_t ne_north;

  char     name[20];
};


struct HumminbirdBase::humminbird_trk_point_t {
  int16_t  deltaeast;
  int16_t  deltanorth;
  uint16_t depth;		/* in centimeters */
};

struct HumminbirdBase::humminbird_trk_header_old_t {      /* 16 bytes, incl signature */
  /* uint32_t signature; */
  uint16_t trk_num;
  uint16_t zero;
  uint16_t num_points;
  uint16_t unknown;	/* Always zero so far. */
  uint32_t time;		/* a time_t, in UTC */

  int32_t start_east;	/* Start of track */
  int32_t start_north;
  int32_t end_east;	/* end of track */
  int32_t end_north;

};

struct HumminbirdBase::humminbird_trk_point_old_t {
  int16_t  deltaeast;
  int16_t  deltanorth;
};

struct HumminbirdBase::group_header_t {
  uint8_t status;
  uint8_t icon;
  uint16_t depth;
  uint32_t time;		/* a time_t, in UTC */
  uint16_t parent_idx;
  uint16_t reserved1;
  uint16_t first_body_index;
  uint16_t reserved2;
  char name[WPT_NAME_LEN];
};

struct HumminbirdBase::group_body_t {
  uint8_t status;
  uint8_t icon;
  uint16_t next_idx;
  uint16_t item[MAX_ITEMS_PER_GROUP];
};


/* Takes a latitude in degrees,
 * returns a latitude in degrees. */
double
HumminbirdBase::geodetic_to_geocentric_hwr(const double gd_lat)
{
  const double gdr = gd_lat * std::numbers::pi / 180.0;

  return atan(cos2_ae * tan(gdr)) * 180.0 * std::numbers::inv_pi;
}

/* Takes a latitude in degrees,
 * returns a latitude in degrees. */
double
HumminbirdBase::geocentric_to_geodetic_hwr(const double gc_lat)
{
  const double gcr = gc_lat * std::numbers::pi / 180.0;

  return atan(tan(gcr)/cos2_ae) * 180.0 * std::numbers::inv_pi;
}

/* Takes a projected "north" value, returns latitude in degrees. */
double
HumminbirdBase::gudermannian_i1924(const double x)
{
  const double norm_x = x/i1924_equ_axis;

  return atan(sinh(norm_x)) * 180.0 * std::numbers::inv_pi;
}

/* Takes latitude in degrees, returns projected "north" value. */
double
HumminbirdBase::inverse_gudermannian_i1924(const double x)
{
  const double x_r = x/180.0 * std::numbers::pi;
  const double guder = log(tan(std::numbers::pi/4.0 + x_r/2.0));

  return guder * i1924_equ_axis;
}

/*******************************************************************************
* %%%        global callbacks called by gpsbabel main process              %%% *
*******************************************************************************/

void
HumminbirdBase::humminbird_rd_init(const QString& fname)
{
  fin_ = gbfopen_be(fname, "rb", MYNAME);

  wpt_num_to_wpt_hash.clear();
}

void
HumminbirdBase::humminbird_rd_deinit() const
{
  gbfclose(fin_);
}

void
HumminbirdBase::humminbird_read_wpt(gbfile* fin)
{
  humminbird_waypt_t w{};

  if (! gbfread(&w, 1, sizeof(w), fin)) {
    fatal(MYNAME ": Unexpected end of file!\n");
  }

  /* Fix endianness - these are now BE */
  w.num = be_read16(&w.num);
  w.zero = be_read16(&w.zero);
  w.depth = be_read16(&w.depth);
  w.time = be_read32(&w.time);
  w.north = be_read32(&w.north);
  w.east = be_read32(&w.east);

  /* All right! Copy the data to the gpsbabel struct... */

  auto* wpt = new Waypoint;

  wpt->shortname = QByteArray(w.name, static_cast<int>(qstrnlen(w.name, sizeof(w.name))));
  wpt->SetCreationTime(w.time);

  double guder = gudermannian_i1924(w.north);
  wpt->latitude = geocentric_to_geodetic_hwr(guder);
  wpt->longitude = static_cast<double>(w.east) / EAST_SCALE * 180.0;

  wpt->altitude  = 0.0; /* It's from a fishfinder... */

  if (w.depth != 0) {
    wpt->set_depth(static_cast<double>(w.depth) / 100.0);
  }

  int num_icons = std::size(humminbird_icons);
  if (w.icon < num_icons) {
    wpt->icon_descr = humminbird_icons[w.icon];
  }

  // In newer versions, this is an enum (though it looks like a bitfield)
  // that describes a sub-status
  switch (w.status) {
  case 0: // Waypoint not used.  So why do we have one?
    delete wpt;
    break;
  case 1: // Waypoint permanent.
  case 2: // Waypoint temporary.
  case 3: // Waypoint man-overboard.
    waypt_add(wpt);
    /* register the point over his internal Humminbird "Number" */
    wpt_num_to_wpt_hash[w.num] = wpt;
    break;
  case 16: // Waypoint group header.
  case 17: // Waypoint group body.
  case 63: // Waypoint group invalid.
  default:
    delete wpt;
    break;
  }
}

void
HumminbirdBase::humminbird_read_route(gbfile* fin) const
{

  humminbird_rte_t hrte{};

  if (! gbfread(&hrte, 1, sizeof(hrte), fin)) {
    fatal(MYNAME ": Unexpected end of file!\n");
  }

  hrte.time = be_read32(&hrte.time);
  hrte.num = be_read16(&hrte.num);

  if (hrte.count > 0) {
    route_head* rte = nullptr;

    for (int i = 0; i < hrte.count; i++) {
      hrte.points[i] = be_read16(&hrte.points[i]);

      /* locate the point over his internal Humminbird "Number" */
      if (const Waypoint* wpt = wpt_num_to_wpt_hash.value(hrte.points[i], nullptr); wpt != nullptr) {
        if (rte == nullptr) {
          rte = new route_head;
          route_add_head(rte);
          rte->rte_name = QByteArray(hrte.name, static_cast<int>(qstrnlen(hrte.name, sizeof(hrte.name))));
        }
        route_add_wpt(rte, new Waypoint(*wpt));
      }
    }
  }
}

void
HumminbirdBase::humminbird_read_track(gbfile* fin)
{

  humminbird_trk_header_t th{};

  if (! gbfread(&th, 1, sizeof(th), fin)) {
    fatal(MYNAME ": Unexpected end of file reading header!\n");
  }

  th.trk_num     = be_read16(&th.trk_num);
  th.num_points  = be_read16(&th.num_points);
  th.time        = be_read32(&th.time);

  th.start_east  = be_read32(&th.start_east);
  th.start_north = be_read32(&th.start_north);
  th.end_east    = be_read32(&th.end_east);
  th.end_north   = be_read32(&th.end_north);

  th.sw_east     = be_read32(&th.sw_east);
  th.sw_north    = be_read32(&th.sw_north);
  th.ne_east     = be_read32(&th.ne_east);
  th.ne_north    = be_read32(&th.ne_north);

  int max_points = (131080 - sizeof(uint32_t) - sizeof(th)) / sizeof(humminbird_trk_point_t);

  if (th.num_points == max_points + 1) {
    th.num_points--;
  }

  if (th.num_points > max_points) {
    fatal(MYNAME ": Too many track points! (%d)\n", th.num_points);
  }

  /* num_points is actually one too big, because it includes the value in
     the header. But we want the extra point at the end because the
     freak-value filter below looks at points[i+1] */
  auto* points = new humminbird_trk_point_t[th.num_points]();
  if (! gbfread(points, sizeof(humminbird_trk_point_t), th.num_points-1, fin)) {
    fatal(MYNAME ": Unexpected end of file reading points!\n");
  }

  int32_t accum_east = th.start_east;
  int32_t accum_north = th.start_north;

  auto* trk = new route_head;
  track_add_head(trk);

  trk->rte_name = QByteArray(th.name, static_cast<int>(qstrnlen(th.name, sizeof(th.name))));
  trk->rte_num  = th.trk_num;

  /* We create one wpt for the info in the header */

  auto* first_wpt = new Waypoint;
  double g_lat = gudermannian_i1924(accum_north);
  first_wpt->latitude  = geocentric_to_geodetic_hwr(g_lat);
  first_wpt->longitude = accum_east/EAST_SCALE * 180.0;
  first_wpt->altitude  = 0.0;
  /* No depth info in the header. */
  track_add_wpt(trk, first_wpt);

  for (int i = 0 ; i < th.num_points-1 ; i++) {
    auto* wpt = new Waypoint;

    points[i].depth      = be_read16(&points[i].depth);
    points[i].deltaeast  = be_read16(&points[i].deltaeast);
    points[i].deltanorth = be_read16(&points[i].deltanorth);

    /* Every once in a while the delta values are
       32767 followed by -32768. Filter that. */

    int16_t next_deltaeast = be_read16(&points[i+1].deltaeast);
    if (points[ i ].deltaeast ==  32767 &&
        next_deltaeast        == -32768) {
      points[ i ].deltaeast = -1;
      points[i+1].deltaeast =  0; /* BE 0 == LE 0 */
    }
    int16_t next_deltanorth = be_read16(&points[i+1].deltanorth);
    if (points[ i ].deltanorth ==  32767 &&
        next_deltanorth        == -32768) {
      points[ i ].deltanorth = -1;
      points[i+1].deltanorth =  0;
    }

    accum_east  += points[i].deltaeast;
    accum_north += points[i].deltanorth;

    double guder = gudermannian_i1924(accum_north);
    wpt->latitude  = geocentric_to_geodetic_hwr(guder);
    wpt->longitude = accum_east/EAST_SCALE * 180.0;
    wpt->altitude  = 0.0;

    if (points[i].depth != 0) {
      wpt->set_depth(static_cast<double>(points[i].depth) / 100.0);
    }

    if (i == th.num_points-2 && th.time != 0) {
      /* Last point. Add the date from the header. */
      /* Unless it's zero. Sometimes happens, possibly if
         the gps didn't have a lock when the track was
         saved. */
      wpt->SetCreationTime(th.time);
    }
    track_add_wpt(trk, wpt);
  }
  delete[] points;
}

void
HumminbirdBase::humminbird_read_track_old(gbfile* fin)
{

  humminbird_trk_header_old_t th{};
  constexpr int file_len = 8048;
  char namebuf[TRK_NAME_LEN];


  if (! gbfread(&th, 1, sizeof(th), fin)) {
    fatal(MYNAME ": Unexpected end of file reading header!\n");
  }

  th.trk_num     = be_read16(&th.trk_num);
  th.num_points  = be_read16(&th.num_points);
  th.time        = be_read32(&th.time);

  th.start_east  = be_read32(&th.start_east);
  th.start_north = be_read32(&th.start_north);
  th.end_east    = be_read32(&th.end_east);
  th.end_north   = be_read32(&th.end_north);

  // These files are always 8048 bytes long. Note that that's the value
  // of the second 16-bit word in the signature.
  int max_points = (file_len - (sizeof(th) + sizeof(uint32_t) + TRK_NAME_LEN)) / sizeof(humminbird_trk_point_old_t);

  if (th.num_points > max_points) {
    fatal(MYNAME ": Too many track points! (%d)\n", th.num_points);
  }

  /* num_points is actually one too big, because it includes the value in
     the header. But we want the extra point at the end because the
     freak-value filter below looks at points[i+1] */
  auto* points = new humminbird_trk_point_old_t[th.num_points]();
  if (! gbfread(points, sizeof(humminbird_trk_point_old_t), th.num_points-1, fin)) {
    fatal(MYNAME ": Unexpected end of file reading points!\n");
  }

  int32_t accum_east = th.start_east;
  int32_t accum_north = th.start_north;

  auto* trk = new route_head;
  track_add_head(trk);

  /* The name is not in the header, but at the end of the file.
     (The last 20 bytes.) */

  gbfseek(fin, file_len-TRK_NAME_LEN, SEEK_SET);
  gbfread(&namebuf, 1, TRK_NAME_LEN, fin);

  trk->rte_name = QByteArray(namebuf, static_cast<int>(qstrnlen(namebuf, sizeof(namebuf))));
  trk->rte_num  = th.trk_num;

  /* We create one wpt for the info in the header */

  auto* first_wpt = new Waypoint;
  double g_lat = gudermannian_i1924(accum_north);
  first_wpt->latitude  = geocentric_to_geodetic_hwr(g_lat);
  first_wpt->longitude = accum_east/EAST_SCALE * 180.0;
  first_wpt->altitude  = 0.0;
  track_add_wpt(trk, first_wpt);

  for (int i = 0 ; i<th.num_points-1 ; i++) {
    auto* wpt = new Waypoint;

    points[i].deltaeast  = be_read16(&points[i].deltaeast);
    points[i].deltanorth = be_read16(&points[i].deltanorth);

//		I've commented this out, don't know if it happens in this
//		format. It happens in the newer version though.

//		/* Every once in a while the delta values are
//		   32767 followed by -32768. Filter that. */
//
//		next_deltaeast = be_read16(&points[i+1].deltaeast);
//		if (points[ i ].deltaeast ==  32767 &&
//		    next_deltaeast        == -32768) {
//			points[ i ].deltaeast = -1;
//			points[i+1].deltaeast =  0; /* BE 0 == LE 0 */
//		}
//		next_deltanorth = be_read16(&points[i+1].deltanorth);
//		if (points[ i ].deltanorth ==  32767 &&
//		    next_deltanorth        == -32768) {
//			points[ i ].deltanorth = -1;
//			points[i+1].deltanorth =  0;
//		}
//
    accum_east  += points[i].deltaeast;
    accum_north += points[i].deltanorth;

    double guder = gudermannian_i1924(accum_north);
    wpt->latitude  = geocentric_to_geodetic_hwr(guder);
    wpt->longitude = accum_east/EAST_SCALE * 180.0;
    wpt->altitude  = 0.0;

    if (i == th.num_points-2 && th.time != 0) {
      /* Last point. Add the date from the header. */
      /* Unless it's zero. Sometimes happens, possibly if
         the gps didn't have a lock when the track was
         saved. */
      wpt->SetCreationTime(th.time);
    }
    track_add_wpt(trk, wpt);
  }
  delete[] points;
}

void
HumminbirdBase::humminbird_read()
{
  while (! gbfeof(fin_)) {
    switch (uint32_t signature = gbfgetuint32(fin_)) {
    case WPT_MAGIC:
    case WPT_MAGIC2:
      humminbird_read_wpt(fin_);
      break;
    case RTE_MAGIC:
      humminbird_read_route(fin_);
      break;
    case TRK_MAGIC:
      humminbird_read_track(fin_);
      return; /* Don't continue. The rest of the file is all zeores */
    case TRK_MAGIC2:
      humminbird_read_track_old(fin_);
      return; /* Don't continue. The rest of the file is all zeores */
    default:
      fatal(MYNAME ": Invalid record header \"0x%08X\" (no or unknown humminbird file)!\n", signature);
    }
  }
}


/************************************************************************************************/

void
HumminbirdBase::humminbird_wr_init(const QString& fname)
{
  fout_ = gbfopen_be(fname, "wb", MYNAME);

  wptname_sh = new MakeShort;

  wptname_sh->set_length(WPT_NAME_LEN - 1);
  wptname_sh->set_badchars(BAD_CHARS);
  wptname_sh->set_mustupper(false);
  wptname_sh->set_mustuniq(false);
  wptname_sh->set_whitespace_ok(true);
  wptname_sh->set_repeating_whitespace_ok(true);
  wptname_sh->set_defname("WPT");

  rtename_sh = new MakeShort;
  rtename_sh->set_length(RTE_NAME_LEN - 1);
  rtename_sh->set_badchars(BAD_CHARS);
  rtename_sh->set_mustupper(false);
  rtename_sh->set_mustuniq(false);
  rtename_sh->set_whitespace_ok(true);
  rtename_sh->set_repeating_whitespace_ok(true);
  rtename_sh->set_defname("Route");

  trkname_sh = new MakeShort;
  trkname_sh->set_length(RTE_NAME_LEN - 1);
  trkname_sh->set_badchars(BAD_CHARS);
  trkname_sh->set_mustupper(false);
  trkname_sh->set_mustuniq(false);
  trkname_sh->set_whitespace_ok(true);
  trkname_sh->set_repeating_whitespace_ok(true);
  trkname_sh->set_defname("Track");

  waypoint_num = 0;
  rte_num_ = 0;

  wpt_id_to_wpt_num_hash.clear();
}

void
HumminbirdBase::humminbird_wr_deinit()
{
  delete wptname_sh;
  wptname_sh = nullptr;
  delete rtename_sh;
  rtename_sh = nullptr;
  delete trkname_sh;
  trkname_sh = nullptr;
  gbfclose(fout_);
}

void
HumminbirdFormat::humminbird_write_waypoint(const Waypoint* wpt)
{
  humminbird_waypt_t hum{};
  int num_icons = std::size(humminbird_icons);

  be_write16(&hum.num, waypoint_num++);
  hum.zero   = 0;
  hum.status = 1;
  hum.icon   = 255;

  // Icon....
  if (!wpt->icon_descr.isNull()) {
    for (int i = 0; i < num_icons; i++) {
      if (!wpt->icon_descr.compare(humminbird_icons[i], Qt::CaseInsensitive)) {
        hum.icon = i;
        break;
      }
    }
    if (hum.icon == 255) {	/* no success, no try to find the item in a more comlex name */
      hum.icon = 0;	/* i.e. "Diamond" as part of "Diamond, Green" or "Green Diamond" */
      for (int i = 0; i < num_icons; i++) {
        if (wpt->icon_descr.contains(humminbird_icons[i], Qt::CaseInsensitive)) {
          hum.icon = i;
          break;
        }
      }
    }
  }

  hum.depth = qRound(wpt->depth_value_or(0) * 100.0);
  be_write16(&hum.depth, hum.depth);

  be_write32(&hum.time, wpt->GetCreationTime().toTime_t());

  double east = wpt->longitude / 180.0 * EAST_SCALE;
  be_write32(&hum.east, qRound((east)));

  double lat = geodetic_to_geocentric_hwr(wpt->latitude);
  double north = inverse_gudermannian_i1924(lat);
  be_write32(&hum.north, qRound(north));

  QString name = (global_opts.synthesize_shortnames)
                 ? wptname_sh->mkshort_from_wpt(wpt)
                 : wptname_sh->mkshort(wpt->shortname);
  strncpy(hum.name, CSTR(name), sizeof(hum.name)-1);

  gbfputuint32(WPT_MAGIC, fout_);
  gbfwrite(&hum, sizeof(hum), 1, fout_);
}

void
HumminbirdHTFormat::humminbird_track_head(const route_head* trk)
{
  int max_points = (131080 - sizeof(uint32_t)- sizeof(humminbird_trk_header_t)) / sizeof(humminbird_trk_point_t);

  trk_head = nullptr;
  last_time = 0;
  if (!trk->rte_waypt_empty()) {
    trk_head = new humminbird_trk_header_t();
    trk_points = new humminbird_trk_point_t[max_points]();
    QString name = trkname_sh->mkshort(trk->rte_name);
    strncpy(trk_head->name, CSTR(name), sizeof(trk_head->name)-1);
    be_write16(&trk_head->trk_num, trk->rte_num);
  }
}

void
HumminbirdHTFormat::humminbird_track_tail(const route_head* /*unused*/)
{
  int max_points = (131080 - sizeof(uint32_t)- sizeof(humminbird_trk_header_t)) / sizeof(humminbird_trk_point_t);

  if (trk_head == nullptr) {
    return;
  }

  be_write32(&trk_head->end_east, last_east);
  be_write32(&trk_head->end_north, last_north);
  be_write32(&trk_head->time, last_time);

  /* Fix some endianness */

  be_write32(&trk_head->sw_east, trk_head->sw_east);
  be_write32(&trk_head->ne_east, trk_head->ne_east);
  be_write32(&trk_head->sw_north, trk_head->sw_north);
  be_write32(&trk_head->ne_north, trk_head->ne_north);

  be_write16(&trk_head->num_points, trk_head->num_points);

  /* Actually write it out */


  gbfputuint32(TRK_MAGIC, fout_);
  gbfwrite(trk_head, 1, sizeof(humminbird_trk_header_t), fout_);
  gbfwrite(trk_points, max_points, sizeof(humminbird_trk_point_t), fout_);
  gbfputuint16(0, fout_); /* Odd but true. The format doesn't fit an int nr of entries. */

  delete trk_head;
  delete[] trk_points;

  trk_head   = nullptr;
  trk_points = nullptr;
}

void
HumminbirdHTFormat::humminbird_track_cb(const Waypoint* wpt)
{
  if (trk_head == nullptr) {
    return;
  }

  int i = trk_head->num_points;

  int32_t east = qRound(wpt->longitude / 180.0 * EAST_SCALE);
  double lat = geodetic_to_geocentric_hwr(wpt->latitude);
  int32_t north = qRound(inverse_gudermannian_i1924(lat));

  if (wpt->creation_time.isValid()) {
    last_time = wpt->GetCreationTime().toTime_t();
  }

  if (i == 0) {
    /* It's the first point. That info goes in the header. */

    be_write32(&trk_head->start_east, east);
    be_write32(&trk_head->start_north, north);

    /* Bounding box. Easy for one point. */
    /* These are not BE yet, fixed in the end. */
    trk_head->sw_east = east;
    trk_head->ne_east = east;
    trk_head->sw_north = north;
    trk_head->ne_north = north;

    /* No depth info in the header. */
  } else {
    /* These points are 16-bit differential. */
    int j = i-1;
    trk_points[j].deltaeast = static_cast<int16_t>(east - last_east);
    trk_points[j].deltanorth =  static_cast<int16_t>(north - last_north);
    trk_points[j].depth = qRound(wpt->depth_value_or(0) * 100.0);

    /* BE-ify */
    be_write16(&trk_points[j].deltaeast, trk_points[j].deltaeast);
    be_write16(&trk_points[j].deltanorth, trk_points[j].deltanorth);
    be_write16(&trk_points[j].depth, trk_points[j].depth);

    /* Update bounding box in header if necessary */
    if (east > trk_head->ne_east) {
      trk_head->ne_east = east;
    }
    if (east < trk_head->sw_east) {
      trk_head->sw_east = east;
    }
    if (north > trk_head->ne_north) {
      trk_head->ne_north = north;
    }
    if (north < trk_head->sw_north) {
      trk_head->sw_north = north;
    }
  }

  last_east = east;
  last_north = north;

  trk_head->num_points++;
}


void
HumminbirdHTFormat::write()
{
  auto humminbird_track_head_lambda = [this](const route_head* rte)->void {
    humminbird_track_head(rte);
  };
  auto humminbird_track_tail_lambda = [this](const route_head* rte)->void {
    humminbird_track_tail(rte);
  };
  auto humminbird_track_cb_lambda = [this](const Waypoint* waypointp)->void {
    humminbird_track_cb(waypointp);
  };
  track_disp_all(humminbird_track_head_lambda, humminbird_track_tail_lambda, humminbird_track_cb_lambda);
}

void
HumminbirdFormat::humminbird_rte_head(const route_head* rte)
{
  humrte = nullptr;
  if (!rte->rte_waypt_empty()) {
    humrte = new humminbird_rte_t();
  }
}

void
HumminbirdFormat::humminbird_rte_tail(const route_head* rte)
{
  if (humrte == nullptr) {
    return;
  }

  if (humrte->count > 0) {
    humrte->num = rte_num_++;
    humrte->time = gpsbabel_time;
    for (int i = 0; i < humrte->count; i++) {
      be_write16(&humrte->points[i], humrte->points[i]);
    }

    be_write16(&humrte->num, humrte->num);
    be_write32(&humrte->time, humrte->time);

    QString name = rtename_sh->mkshort(rte->rte_name);
    strncpy(humrte->name, CSTR(name), sizeof(humrte->name)-1);

    gbfputuint32(RTE_MAGIC, fout_);
    gbfwrite(humrte, sizeof(*humrte), 1, fout_);
  }

  delete humrte;
  humrte = nullptr;
}

QString HumminbirdFormat::wpt_to_id(const Waypoint* wpt)
{
  QString id = QStringLiteral("%1\01%2\01%3").arg(wpt->shortname)
                .arg(wpt->latitude, 0, 'f', 9).arg(wpt->longitude, 0, 'f', 9);
  return id;
}

void
HumminbirdFormat::humminbird_write_rtept(const Waypoint* wpt) const
{
  if (humrte == nullptr) {
    return;
  }
  QString id = wpt_to_id(wpt);

  if (!wpt_id_to_wpt_num_hash.contains(id)) {
    // This should not occur, we just scanned all waypoints and routes.
    warning("Missing waypoint reference in route, point dropped from route.");
    return;
  }

  if (humrte->count < MAX_RTE_POINTS) {
    humrte->points[humrte->count] = wpt_id_to_wpt_num_hash.value(id);
    humrte->count++;
  } else {
    warning(MYNAME ": Sorry, routes are limited to %d points!\n", MAX_RTE_POINTS);
    fatal(MYNAME ": You can use our simplify filter to reduce the number of route points.\n");
  }
}

void
HumminbirdFormat::humminbird_write_waypoint_wrapper(const Waypoint* wpt)
{
  QString id = wpt_to_id(wpt);
  if (!wpt_id_to_wpt_num_hash.contains(id)) {
    wpt_id_to_wpt_num_hash[id] = waypoint_num;
    humminbird_write_waypoint(wpt);
  }
}

void
HumminbirdFormat::write()
{
  auto humminbird_write_waypoint_wrapper_lambda = [this](const Waypoint* waypointp)->void {
    humminbird_write_waypoint_wrapper(waypointp);
  };
  waypt_disp_all(humminbird_write_waypoint_wrapper_lambda);
  route_disp_all(nullptr, nullptr, humminbird_write_waypoint_wrapper_lambda);

  auto humminbird_rte_head_lambda = [this](const route_head* rte)->void {
    humminbird_rte_head(rte);
  };
  auto humminbird_rte_tail_lambda = [this](const route_head* rte)->void {
    humminbird_rte_tail(rte);
  };
  auto humminbird_write_rtept_lambda = [this](const Waypoint* waypointp)->void {
    humminbird_write_rtept(waypointp);
  };
  route_disp_all(humminbird_rte_head_lambda, humminbird_rte_tail_lambda, humminbird_write_rtept_lambda);
}
