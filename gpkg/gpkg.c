/******************************************************************************
**
** This "SQLite extension" implements the minimum functions to create and
** handle a GeoPackage from version 1.2 to version 1.4.
** It handles this extensions :
**
******************************************************************************
**
** Version history
** 1.0.0 - 2020-12-17 - Initial version
** 1.0.1 - 2021-03-18 - Corrected bug in isEmptyGPKGGeometry
** 1.0.2 - 2021-05-01 - Added support for version 1.3
** 1.0.3 - 2021-06-29 - Bug where removing an GPKG extension
** 1.0.4 - 2026-02-09 - Support GeoPackage 1.4
** 1.0.5 - 2026-02-21 - Correction of the content of gpkg_extensions.definition when creating spatial indexes (gpkg_rtree_index)
**
******************************************************************************/

#include <string.h>
#include <math.h>
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

// Uncomment this define to always use the GPKG Binary Header to get the Geometry envelope and check for the empty geometry
// #define GPKG_ALLWAYS_USE_HEADER

// Version of this extension
#define VERSION "1.0.5"

// Application ID
#define GPKG_APPLICATION_ID 1196444487

// GPKG Versions
#define GPKG_VERSION_10200 10200
#define GPKG_VERSION_10300 10300
#define GPKG_VERSION_10400 10400

// GeoPackage header constants
#define GPKG_MAGIC1 0x47
#define GPKG_MAGIC2 0x50
#define GPKG_VERSION 0x00
#define GPKG_BINARY_TYPE_BIT (0x01 << 5) // 0x20 (00100000)
#define GPKG_EMPTY_BIT (0x01 << 4) // 0x10 (00010000)
#define GPKG_ENV_BITS (0x07 << 1) // 0x0E (00001110)
#define GPKG_BYTEORDER_BIT 0x01 // (00000001)

// Constans to check ENDIANESS
static const unsigned char LITTLE_ENDIAN = (unsigned char)1;
static const unsigned char BIG_ENDIAN = (unsigned char)0;

// Geometry types
#define wkbGeometry 0
#define wkbPoint 1
#define wkbLineString 2
#define wkbPolygon 3
#define wkbMultiPoint 4
#define wkbMultiLineString 5
#define wkbMultiPolygon 6
#define wkbGeometryCollection 7

// Geometry type names (accepts GEOMETRYCOLLECTION as a synonym of GEOMCOLLECTION)
static const char* wktGeomtryTypes[] = { "GEOMETRY", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON", "GEOMCOLLECTION", "GEOMETRYCOLLECTION", NULL };

// Ordinates
#define X 0
#define Y 1
#define Z 2
#define M 3

// Functions (min or max)
#define MIN 0
#define MAX 1

// "fordward" declarations
static int readWKBGeometryEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int ordinate, int maxmin, int geometryTypeExpected, double* res);
static int isEmptyWKBGeometry(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int geometryTypeExpected);

// Utility function that executes the SQL in "sql" using sqlite3_exec.
// If it fails it executes the SQL in "errsql" using sqlite3_exec as long as it is not NULL
// Releases the "sql" and "errsql" strings.
// This is so that the function that calls "sqlite3_exec_free" can make a return directly.
// Usage example in fnct_GPKGAddSpatialIndex.
// context -> sqlite3_context
// db -> sqlite3
// sql -> SQL statement to execute
// errsql -> SQL statement to execute if the previous one failed
// Returns the result of "sqlite3_exec(sql)"
static int sqlite3_exec_free(sqlite3_context* context, sqlite3* db, char* sql, char* errsql)
{
    int res;
    char* err = NULL;

    res = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (res != SQLITE_OK)
    {
        if (errsql != NULL)
            sqlite3_exec(db, errsql, NULL, NULL, NULL);
        sqlite3_result_error(context, err, -1);
        sqlite3_free(err);
    }
    sqlite3_free(sql);
    sqlite3_free(errsql); // It's ok to call sqlite3_free(NULL). It's a noop.
    return res;
}

// Returns CPU ENDIANESS
static unsigned char endian()
{
    volatile unsigned int i = 0x01234567;
    if ((*((unsigned char*)(&i))) == 0x67)
        return LITTLE_ENDIAN;
    else
        return BIG_ENDIAN;
}

// Reads a 4 byte int from a byte array
// p_blob -> byte array to read from
// index -> start read position that gets atvanced 4 bytes
// byteOrder -> ENDIANESS
// Returns the int readed
static int getInt(unsigned char* p_blob, int* index, unsigned char byteOrder)
{
    int res;

    if (byteOrder == endian())
        res = *(int*)&p_blob[*index];
    else
    {
        unsigned char bytes[4] = { 0, 0, 0, 0 }; // Initialized to remove the LNT1006 message from Visual C
        bytes[0] = p_blob[*index + 3];
        bytes[1] = p_blob[*index + 2];
        bytes[2] = p_blob[*index + 1];
        bytes[3] = p_blob[*index];
        res = *(int*)bytes;
    }
    *index += 4;
    return res;
}

// Reads an 8 byte double from a byte array
// p_blob -> byte array to read from
// index -> start read position that gets atvanced 8 bytes
// byteOrder -> ENDIANESS
// Returns the double readed
static double getDouble(unsigned char* p_blob, int* index, unsigned char byteOrder)
{
    double res;

    if (byteOrder == endian())
        res = *(double*)&p_blob[*index];
    else
    {
        unsigned char bytes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }; // Initialized to remove the LNT1006 message from Visual C
        bytes[0] = p_blob[*index + 7];
        bytes[1] = p_blob[*index + 6];
        bytes[2] = p_blob[*index + 5];
        bytes[3] = p_blob[*index + 4];
        bytes[4] = p_blob[*index + 3];
        bytes[5] = p_blob[*index + 2];
        bytes[6] = p_blob[*index + 1];
        bytes[7] = p_blob[*index];
        res = *(double*)bytes;
    }
    *index += 8;
    return res;
}

// Leave in the "res" the ordinate "ordinate" of a Point (or what is the same in this context, a coordinate)
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// dimension -> Dimensions of the coordinates of the geometry
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBPointOrd(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension, int ordinate, double* res)
{
    if (*index + (dimension * 8) > n_bytes)
        return 0;
    *index += ordinate * 8;
    *res = getDouble(p_blob, index, byteOrder);
    *index += (dimension - ordinate - 1) * 8;
    return 1;
}

// Check if a Point is empty (In WKB a Point cannot be empty, but as we are processinbg GPKG geometries we are considering empty a point with all its ordinates equal to NaN+)
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
static int isEmptyWKBPoint(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    if (*index + (dimension * 8) > n_bytes)
        return -1;
    for (int i = 0; i < dimension; i++)
    {
        if (!isnan(getDouble(p_blob, index, byteOrder)))
        {
            *index += (dimension - i - 1) * 8; // Advance to the end of the Point. -1 is because getDouble has already advanced the index
            return 0; // Is not empty
        }
    }
    return 1; // Is empty
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a LineString
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// dimension -> Dimensions of the coordinates of the geometry
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBLineStringEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension, int ordinate, int maxmin, double* res)
{
    int numPoints = getInt(p_blob, index, byteOrder);
    if (numPoints < 1)
        return 0;
    if (*index + numPoints * dimension * 8 > n_bytes)
        return 0;
    if (!readWKBPointOrd(p_blob, n_bytes, index, byteOrder, dimension, ordinate, res)) // Read 1st point (coordinate)
        return 0;
    double res2;
    for (int i = 1; i < numPoints; i++)
    {
        if (!readWKBPointOrd(p_blob, n_bytes, index, byteOrder, dimension, ordinate, &res2))
            return 0;
        if (maxmin == MIN)
        {
            if (res2 < *res)
                *res = res2;
        }
        else //if (maxmin == MAX)
        {
            if (res2 > *res)
                *res = res2;
        }
    }
    return 1;
}

// Check if a LinesString is empty
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
// An empty LinesString has no points
static int isEmptyWKBLineString(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    int numPoints = getInt(p_blob, index, byteOrder);
    if (numPoints < 1)
        return 1; // Is empty
    if (*index + numPoints * dimension * 8 > n_bytes)
        return -1;
    *index += numPoints * dimension * 8;
    return 0; // Is not empty
}

// Adavnces "index" to the end of the LineString
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 0 if there is an error or 1 if it's correct
static int skipWKBLineString(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    int numPoints = getInt(p_blob, index, byteOrder);
    if (numPoints < 1)
        return 0;
    if (*index + numPoints * dimension * 8 > n_bytes)
        return 0;
    *index += numPoints * dimension * 8;
    return 1;
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a Polygon
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// dimension -> Dimensions of the coordinates of the geometry
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBPolygonEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension, int ordinate, int maxmin, double* res)
{
    int numRings = getInt(p_blob, index, byteOrder);
    if (numRings < 1)
        return 0;
    if (!readWKBLineStringEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, res))
        return 0;
    // FOr X andy Y ordinates there is no need to look at the inner rings but we need to advance the index in case we come from a MultiPolygon
    if (ordinate < Z)
    {
        for (int i = 1; i < numRings; i++)
        {
            if (!skipWKBLineString(p_blob, n_bytes, index, byteOrder, dimension))
                return 0;
        }
    }
    else
    {
        double res2;
        for (int i = 1; i < numRings; i++)
        {
            if (!readWKBLineStringEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, &res2))
                return 0;
            if (maxmin == MIN)
            {
                if (res2 < *res)
                    *res = res2;
            }
            else //if (maxmin == MAX)
            {
                if (res2 > *res)
                    *res = res2;
            }
        }
    }
    return 1;
}

// Check if a Polygon is empty
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
// An empty Polygon has only an exterior ring that is empty
static int isEmptyWKBPolygon(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    int numRings = getInt(p_blob, index, byteOrder);
    if (numRings < 1)
        return -1;
    if (numRings == 1)
        return isEmptyWKBLineString(p_blob, n_bytes, index, byteOrder, dimension); // Is empty if the exterior ring is empty
    for (int i = 0; i < numRings; i++)
    {
        if (!skipWKBLineString(p_blob, n_bytes, index, byteOrder, dimension))
            return -1;
    }
    return 0; // Is not empty
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a MultiPoint
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// dimension -> Dimensions of the coordinates of the geometry
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBMultiPointEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension, int ordinate, int maxmin, double* res)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 0;
    if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbPoint, res)) // Read 1st Point
        return 0;
    double res2;
    for (int i = 1; i < numGeoms; i++)
    {
        if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbPoint, &res2))
            return 0;
        if (maxmin == MIN)
        {
            if (res2 < *res)
                *res = res2;
        }
        else //if (maxmin == MAX)
        {
            if (res2 > *res)
                *res = res2;
        }
    }
    return 1;
}

// Check if a MultiPoint is empty
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
// All the components of an empty MultiPoint are empty
static int isEmptyWKBMultiPoint(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 1;
    int res = 1;
    for (int i = 0; i < numGeoms; i++)
    {
        int res2 = isEmptyWKBGeometry(p_blob, n_bytes, index, byteOrder, wkbPoint);
        if (res2 < 0)
            return res2;
        if (res2 == 0)
            res = res2;
    }
    return res;
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a MultiLineString
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// dimension -> Dimensions of the coordinates of the geometry
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBMultiLineStringEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension, int ordinate, int maxmin, double* res)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 0;
    if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbLineString, res)) // Read 1st LineString
        return 0;
    double res2;
    for (int i = 1; i < numGeoms; i++)
    {
        if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbLineString, &res2))
            return 0;
        if (maxmin == MIN)
        {
            if (res2 < *res)
                *res = res2;
        }
        else //if (maxmin == MAX)
        {
            if (res2 > *res)
                *res = res2;
        }
    }
    return 1;
}

// Check if a MultiLineString is empty
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
// All the components of an empty MultiLineString are empty
static int isEmptyWKBMultiLineString(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 1;
    int res = 1;
    for (int i = 0; i < numGeoms; i++)
    {
        int res2 = isEmptyWKBGeometry(p_blob, n_bytes, index, byteOrder, wkbLineString);
        if (res2 < 0)
            return res2;
        if (res2 == 0)
            res = res2;
    }
    return res;
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a MultiPolygon
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// dimension -> Dimensions of the coordinates of the geometry
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBMultiPolygonEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension, int ordinate, int maxmin, double* res)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 0;
    if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbPolygon, res)) // Read 1st polygon
        return 0;
    double res2;
    for (int i = 1; i < numGeoms; i++)
    {
        if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbPolygon, &res2))
            return 0;
        if (maxmin == MIN)
        {
            if (res2 < *res)
                *res = res2;
        }
        else //if (maxmin == MAX)
        {
            if (res2 > *res)
                *res = res2;
        }
    }
    return 1;
}

// Check if a MultiPolygon is empty
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
// All the components of an empty MultiPolygon are empty
static int isEmptyWKBMultiPolygon(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 1;
    int res = 1;
    for (int i = 0; i < numGeoms; i++)
    {
        int res2 = isEmptyWKBGeometry(p_blob, n_bytes, index, byteOrder, wkbPolygon);
        if (res2 < 0)
            return res2;
        if (res2 == 0)
            res = res2;
    }
    return res;
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a GeometryCollection
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// dimension -> Dimensions of the coordinates of the geometry
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBGeometryCollectionEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension, int ordinate, int maxmin, double* res)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 0;
    if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbGeometry, res)) // Read 1st geometry
        return 0;
    double res2;
    for (int i = 1; i < numGeoms; i++)
    {
        if (!readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, wkbGeometry, &res2))
            return 0;
        if (maxmin == MIN)
        {
            if (res2 < *res)
                *res = res2;
        }
        else //if (maxmin == MAX)
        {
            if (res2 > *res)
                *res = res2;
        }
    }
    return 1;
}

// Check if a GeometryCollection is empty
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// dimension -> Dimensions of the coordinates of the geometry
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
// All the components of an empty GeometryCollecion are empty
static int isEmptyWKBGeometryCollection(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int dimension)
{
    int numGeoms = getInt(p_blob, index, byteOrder);
    if (numGeoms < 1)
        return 1;
    int res = 1;
    for (int i = 0; i < numGeoms; i++)
    {
        int res2 = isEmptyWKBGeometry(p_blob, n_bytes, index, byteOrder, wkbGeometry);
        if (res2 < 0)
            return res2;
        if (res2 == 0)
            res = res2;
    }
    return res;
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a Geometry
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// geometryTypeExpected -> Type of geometry we expect to find. If we put wkbgGeometry it accepts all geometry type.
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readWKBGeometryEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int ordinate, int maxmin, int geometryTypeExpected, double* res)
{
    unsigned char newByteOrder;
    int typeInt;
    int geometryType;
    int dimension;
    int hasZ;
    int hasM;
    int hasSRID;

    // Check the ByteOrder
    newByteOrder = p_blob[(*index)++];
    if (newByteOrder == LITTLE_ENDIAN || newByteOrder == BIG_ENDIAN) // Si no hi ha byteOrder, agafem el que ens venia per paràmetre
        byteOrder = newByteOrder;

    typeInt = getInt(p_blob, index, byteOrder);

    // Check dimensions
    hasZ = ((typeInt & 0x80000000) != 0 || (typeInt & 0xffff) / 1000 == 1 || (typeInt & 0xffff) / 1000 == 3);
    hasM = ((typeInt & 0x40000000) != 0 || (typeInt & 0xffff) / 1000 == 2 || (typeInt & 0xffff) / 1000 == 3);
    dimension = 2 + (hasZ ? 1 : 0) + (hasM ? 1 : 0);
    // HACK! The M can be the 3rd or 4th ordinate. If we want the M but there is no Z we simulate that we want the Z (3rd ordinate)
    if (dimension == 3 && ordinate == M)
        ordinate = Z;
    else if (dimension < ordinate) // Not enough dimensions
        return 0;

    // Check if it has SRID and if it has we skip it
    hasSRID = (typeInt & 0x20000000) != 0;
    if (hasSRID)
    {
        // SRID = getInt(p_blob, *index, byteOrder);
        *index += 4;
    }

    // Check geometry type
    geometryType = (typeInt & 0xffff) % 1000;
    if (geometryTypeExpected != wkbGeometry && geometryTypeExpected != geometryType)
        return 0;
    switch (geometryType)
    {
    case wkbPoint:
        if (!readWKBPointOrd(p_blob, n_bytes, index, byteOrder, dimension, ordinate, res))
            return 0;
        break;

    case wkbLineString:
        if (!readWKBLineStringEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, res))
            return 0;
        break;

    case wkbPolygon:
        if (!readWKBPolygonEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, res))
            return 0;
        break;

    case wkbMultiPoint:
        if (!readWKBMultiPointEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, res))
            return 0;
        break;

    case wkbMultiLineString:
        if (!readWKBMultiLineStringEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, res))
            return 0;
        break;

    case wkbMultiPolygon:
        if (!readWKBMultiPolygonEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, res))
            return 0;
        break;

    case wkbGeometryCollection:
        if (!readWKBGeometryCollectionEnv(p_blob, n_bytes, index, byteOrder, dimension, ordinate, maxmin, res))
            return 0;
        break;

    default:
        return 0;
        break;
    }
    return 1;
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a Geometry looking at the GPKG header
// p_blob -> BLOB with geometry in GPKG format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// ordinate -> Ordinate we want to read
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error reading del GPKG header, 1 if the GPKG header is correct, -1 if the header is correct but has no envelope
static int readGPKGHeaderEnv(unsigned char* p_blob, int n_bytes, int* index, int ordinate, int maxmin, double* res)
{
    unsigned char flags;
    int envelopeType;
    int isEmpty;
    int byteOrder;
    int dimension;

    if (p_blob[(*index)++] != GPKG_MAGIC1)
    {
        return 0; // Not a GPKG BLOB
    }
    if (p_blob[(*index)++] != GPKG_MAGIC2)
    {
        return 0; // Not a GPKG BLOB
    }
    if (p_blob[(*index)++] != GPKG_VERSION)
    {
        return 0; // Only version 1 supported
    }
    flags = p_blob[(*index)++];
    isEmpty = (flags & GPKG_EMPTY_BIT) >> 4;
    // Skip SRID
    *index += 4;
    // Skip envelope Bytes
    envelopeType = (flags & GPKG_ENV_BITS) >> 1;
    if (isEmpty)
    {
        switch (envelopeType)
        {
        case 0: break; // No envelope
        case 1: *index += 32; break; // X,Y envelope
        case 2: *index += 48; break; // X,Y,Z envelope
        case 3: *index += 48; break; // X,Y,M envelope
        case 4: *index += 64;  break; // X,Y,X,M envelope
        default: return 0; // Unknown envelope type
        }
    }
    else
    {
        byteOrder = flags & GPKG_BYTEORDER_BIT;
        switch (envelopeType)
        {
        case 0: return -1; // No envelope
        case 1: dimension = 2; break; // X,Y envelope
        case 2: dimension = 3; break; // X,Y,Z envelope
        case 3: dimension = 3; break; // X,Y,M envelope
        case 4: dimension = 4;  break; // X,Y,X,M envelope
        default: return 0; // Unknown envelope type
        }
        if (ordinate == Z && (envelopeType == 1 || envelopeType == 3))
            return 0; // No Z in the envelope
        if (ordinate == M && (envelopeType == 1 || envelopeType == 2))
            return 0; // No M in the envelope
        // HACK! The M can be the 3rd or 4th ordinate. If we want the M but there is no Z we simulate that we want the Z (3rd ordinate)
        if (ordinate == M && envelopeType == 3)
            ordinate = Z;
        if (maxmin == MIN)
        {
            *index += ordinate * 8;
            *res = getDouble(p_blob, index, byteOrder);
            if (isnan(*res))
            {
                *index += (dimension + dimension - ordinate - 1) * 8;
                return -1; // Was an empty envelope ?
            }
        }
        else //if (maxmin == MAX)
        {
            *index += dimension + ordinate * 8;
            *res = getDouble(p_blob, index, byteOrder);
            if (isnan(*res))
            {
                *index += (dimension - ordinate - 1) * 8;
                return -1; // Was an empty envelope ?
            }
        }
    }
    return 1;
}

// Skips a header in GPKG format
// p_blob -> BLOB with geometry in GPKG format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// Returns 0 if there is an error reading del GPKG header, 1 if the GPKG header is correct
static int skipGPKGHeader(unsigned char* p_blob, int n_bytes, int* index)
{
    unsigned char flags;
    int envelopeType;

    if (p_blob[(*index)++] != GPKG_MAGIC1)
    {
        return 0; // Not a GPKG BLOB
    }
    if (p_blob[(*index)++] != GPKG_MAGIC2)
    {
        return 0; // Not a GPKG BLOB
    }
    if (p_blob[(*index)++] != GPKG_VERSION)
    {
        return 0; // Only version 1 supported
    }
    flags = p_blob[(*index)++];
    // Skip SRID
    *index += 4;
    // Skip envelope Bytes
    envelopeType = (flags & GPKG_ENV_BITS) >> 1;
    switch (envelopeType)
    {
    case 0: break; // No envelope
    case 1: *index += 32; break; // X,Y envelope
    case 2: *index += 48; break; // X,Y,Z envelope
    case 3: *index += 48; break; // X,Y,M envelope
    case 4: *index += 64;  break; // X,Y,X,M envelope
    default: return 0; // Unknown envelope type
    }
    return 1;
}

// Leave in the "res" parameter the maximum or minimum ordinate "ordinate" of a Geometry
// p_blob -> BLOB with geometry in GPKG format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// ordinate -> Ordinate we want to read
// maxmin -> It says if we want the maximum or the minimum of the ordinate
// geometryTypeExpected -> Type of geometry we expect to find. If we put wkbgGeometry it accepts all geometry type.
// res <- Value we requested (maximum or minimum of the ordinate)
// Returns 0 if there is an error or 1 if it's correct
static int readGPKGGeometryEnv(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int ordinate, int maxmin, int geometryTypeExpected, double* res)
{
    int headerOk;

#ifdef GPKG_ALLWAYS_USE_HEADER

    headerOk = readGPKGHeaderEnv(p_blob, n_bytes, index, ordinate, maxmin, res);

#else 

    if (!skipGPKGHeader(p_blob, n_bytes, index))
        return 0; // Error skipping GPKG header
    headerOk = -1; // Force to read the envelope from the geometry coordinates

#endif // GPKG_ALLWAYS_USE_HEADER

    if (headerOk == 1)
        return 1;
    else
        return readWKBGeometryEnv(p_blob, n_bytes, index, byteOrder, ordinate, maxmin, geometryTypeExpected, res);
}

// Check if a GPKG header of a Geometry is flagged as empty
// p_blob -> BLOB with geometry in GPKG format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// isEmpty <- Gets the value of 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
// Returns 0 if there is an error reading the GPKG header or 1 if the GPKG header is correct
static int readEmptyGPKGHeader(unsigned char* p_blob, int n_bytes, int* index, int* isEmpty)
{
    unsigned char flags;
    int envelopeType;

    if (p_blob[(*index)++] != GPKG_MAGIC1)
    {
        *isEmpty = -1;
        return 0; // Not a GPKG BLOB
    }
    if (p_blob[(*index)++] != GPKG_MAGIC2)
    {
        *isEmpty = -1;
        return 0; // Not a GPKG BLOB
    }
    if (p_blob[(*index)++] != GPKG_VERSION)
    {
        *isEmpty = -1;
        return 0; // Only version 1 supported
    }
    flags = p_blob[(*index)++];
    *isEmpty = (flags & GPKG_EMPTY_BIT) >> 4;
    // Skip SRID
    *index += 4;
    // Skip envelope Bytes
    envelopeType = (flags & GPKG_ENV_BITS) >> 1;
    switch (envelopeType)
    {
    case 0: break; // No envelope
    case 1: *index += 32; break; // X,Y envelope
    case 2: *index += 48; break; // X,Y,Z envelope
    case 3: *index += 48; break; // X,Y,M envelope
    case 4: *index += 64;  break; // X,Y,X,M envelope
    default: *isEmpty = -1; return 0; // Unknown envelope type
    }
    return 1; // Ok 
}

// Check if a Geometry is empty
// p_blob -> BLOB with geometry in WKB format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// geometryTypeExpected -> Type of geometry we hope to find. If we put wkbgGeometry it accepts all geometries
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
static int isEmptyWKBGeometry(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int geometryTypeExpected)
{
    unsigned char newByteOrder;
    int typeInt;
    int geometryType;
    int dimension;
    int hasZ;
    int hasM;
    int hasSRID;

    // Check the ByteOrder
    newByteOrder = p_blob[(*index)++];
    if (newByteOrder == LITTLE_ENDIAN || newByteOrder == BIG_ENDIAN) // If the byteOrder is correct we take it, else keep the value of the parameter
        byteOrder = newByteOrder;

    typeInt = getInt(p_blob, index, byteOrder);

    // Check dimensions
    hasZ = ((typeInt & 0x80000000) != 0 || (typeInt & 0xffff) / 1000 == 1 || (typeInt & 0xffff) / 1000 == 3);
    hasM = ((typeInt & 0x40000000) != 0 || (typeInt & 0xffff) / 1000 == 2 || (typeInt & 0xffff) / 1000 == 3);
    dimension = 2 + (hasZ ? 1 : 0) + (hasM ? 1 : 0);

    // Check if it has SRID and if it has we skip it
    hasSRID = (typeInt & 0x20000000) != 0;
    if (hasSRID)
    {
        // SRID = getInt(p_blob, *index, byteOrder);
        *index += 4;
    }

    // Check geometry type
    geometryType = (typeInt & 0xffff) % 1000;
    if (geometryTypeExpected != wkbGeometry && geometryTypeExpected != geometryType)
        return -1;
    switch (geometryType)
    {
    case wkbPoint: return isEmptyWKBPoint(p_blob, n_bytes, index, byteOrder, dimension);
    case wkbLineString: return isEmptyWKBLineString(p_blob, n_bytes, index, byteOrder, dimension);
    case wkbPolygon: return isEmptyWKBPolygon(p_blob, n_bytes, index, byteOrder, dimension);
    case wkbMultiPoint: return isEmptyWKBMultiPoint(p_blob, n_bytes, index, byteOrder, dimension);
    case wkbMultiLineString: return isEmptyWKBMultiLineString(p_blob, n_bytes, index, byteOrder, dimension);
    case wkbMultiPolygon: return isEmptyWKBMultiPolygon(p_blob, n_bytes, index, byteOrder, dimension);
    case wkbGeometryCollection: return isEmptyWKBGeometryCollection(p_blob, n_bytes, index, byteOrder, dimension);
    default: return -1;
    }
}

// Check if a Geometry is empty
// p_blob -> BLOB with geometry in GPKG format
// n_bytes -> Length in bytes of the blob
// index <-> Position where to start reading the BLOB and returns the position where to continue reading
// byteOrder -> ENDIANESS in which the BLOB is stored
// geometryTypeExpected -> Type of geometry we hope to find. If we put wkbgGeometry it accepts all geometries
// Returns 1 if the Geometry is empty, 0 if it is not empty, -1 if there is an error
static int isEmptyGPKGGeometry(unsigned char* p_blob, int n_bytes, int* index, unsigned char byteOrder, int geometryTypeExpected)
{
    int isEmpty = 0;

    // Check GPKG header
    if (!readEmptyGPKGHeader(p_blob, n_bytes, index, &isEmpty))
        return -1;

#ifdef GPKG_ALLWAYS_USE_HEADER

    return isEmpty;

#else 

    if (isEmpty == 1)
        return 1;
    else
        return isEmptyWKBGeometry(p_blob, n_bytes, index, byteOrder, geometryTypeExpected);

#endif // GPKG_ALLWAYS_USE_HEADER

}

// SQL function: ST_MinX(GEOMETRY); 
// Returns the minimum X of a geometry or NULL if there is an error
static void fnct_STMinX(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), X, MIN, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: ST_MinY(GEOMETRY); 
// Returns the minimum Y of a geometry or NULL if there is an error
static void fnct_STMinY(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), Y, MIN, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: ST_MinZ(GEOMETRY); 
// Returns the minimum Z of a geometry or NULL if there is an error
static void fnct_STMinZ(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), Z, MIN, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: ST_MinM(GEOMETRY); 
// Returns the minimum M of a geometry or NULL if there is an error
static void fnct_STMinM(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), M, MIN, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: SQL function: ST_MaxX(GEOMETRY); 
// Returns the maximum X of a geometry or NULL if there is an error
static void fnct_STMaxX(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), X, MAX, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: SQL function: ST_MaxY(GEOMETRY); 
// Returns the maximum Y of a geometry or NULL if there is an error
static void fnct_STMaxY(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), Y, MAX, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: SQL function: ST_MaxZ(GEOMETRY); 
// Returns the maximum Z of a geometry or NULL if there is an error
static void fnct_STMaxZ(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), Z, MAX, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: SQL function: ST_MaxM(GEOMETRY); 
// Returns the maximum M of a geometry or NULL if there is an error
static void fnct_STMaxM(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    double res;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 29) // Not enough bytes (A point without SRID and with 2 dimensions occupies 21 bytes + 8 minimum GPKG header)
        {
            if (readGPKGGeometryEnv(p_blob, n_bytes, &index, endian(), M, MAX, wkbGeometry, &res))
            {
                sqlite3_result_double(context, res);
                return;
            }
        }
    }
    sqlite3_result_null(context);
}

// SQL function: SQL function: ST_IsEmpty(GEOMETRY); 
// Returns 1 if the geometry is empty, 0 if it is not empty, -1 if there is an error (therefore ISEMPTY (GEOM) is evaluated to TRUE)
static void fnct_STIsEmpty(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    unsigned char* p_blob;
    int n_bytes;
    int index = 0;
    int res = -1;

    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) // Must be a BLOB
    {
        p_blob = (unsigned char*)sqlite3_value_blob(argv[0]);
        n_bytes = sqlite3_value_bytes(argv[0]);
        if (n_bytes >= 13) // Not enough bytes (at least 1 for Endianess and 4 for TypeInt + 8 minimum GPKG header)
            res = isEmptyGPKGGeometry(p_blob, n_bytes, &index, endian(), wkbGeometry);
    }
    sqlite3_result_int(context, res);
}

// SQL function: GPKG_AddGeometryColumn(identifier, tableName, geometryColumn, geometryType, srsId, zFlag, mFlag); 
// identifier -> Identifier of the geometry (gpkg_contents)
// tableName -> Name of the table
// geometryColumn -> Column that contains the geometry
// geometryType -> WKT geometry name : "GEOMETRY", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON", "GEOMCOLLECTION"
// srsId -> SRS ID of the geometries
// zFlag -> 0: z values prohibited; 1: z values mandatory; 2: z values optional
// mflag -> 0: m values prohibited; 1: m values mandatory; 2: m values optional
// Populates the gpkg_contents table (if not already present)
// Populates the gpkg_geometry_columns table
// On success returns nothing. If there is an error throw an exception
static void fnct_GPKGAddGeometryColumn(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    const char* identifier;
    const char* table;
    const char* gcolumn;
    const char* gtype;
    int igtype;
    int srsid;
    int zflag;
    int mflag;
    sqlite3* db;
    char* sql;

    // Get the parameters
    identifier = (const char*)sqlite3_value_text(argv[0]);
    table = (const char*)sqlite3_value_text(argv[1]);
    gcolumn = (const char*)sqlite3_value_text(argv[2]);
    gtype = (const char*)sqlite3_value_text(argv[3]);
    srsid = sqlite3_value_int(argv[4]);
    zflag = sqlite3_value_int(argv[5]);
    mflag = sqlite3_value_int(argv[6]);

    // Check paràmeters
    for (igtype = 0; wktGeomtryTypes[igtype] != NULL; igtype++)
    {
        if (_stricmp(gtype, wktGeomtryTypes[igtype]) == 0)
            break;
    }
    if (wktGeomtryTypes[igtype] == NULL)
    {
        sqlite3_result_error(context, "GPKG_AddGeometryColumn() error: argument 3 [geometryType] unrecognised geometry type", -1);
        return;
    }
    // HACK! Convert "GEOMETRYCOLLECTION" to "GEOMCOLLECTION"
    if (igtype == 8)
        igtype = 7;
    if (zflag != 0 && zflag != 1 && zflag != 2)
    {
        sqlite3_result_error(context, "GPKG_AddGeometryColumn() error: argument 5 [zFlag] must be 0, 1 or 2", -1);
        return;
    }
    if (mflag != 0 && mflag != 1 && mflag != 2)
    {
        sqlite3_result_error(context, "GPKG_AddGeometryColumn() error: argument 6 [mFlag] must be 0, 1 or 2", -1);
        return;
    }

    // Get DB handle
    db = sqlite3_context_db_handle(context);

    // Populate gpkg_contents
    sql = sqlite3_mprintf("INSERT OR IGNORE INTO gpkg_contents(table_name, data_type, identifier, srs_id) VALUES(%Q, 'features', %Q, %i)",
        table, identifier, srsid);
    if (sqlite3_exec_free(context, db, sql, NULL) != SQLITE_OK)
        return;

    // Populate gpkg_geometry_column
    sql = sqlite3_mprintf("INSERT INTO gpkg_geometry_columns(table_name, column_name, geometry_type_name, srs_id, z, m) VALUES(%Q, %Q, '%s', %i, %i, %i)",
        table, gcolumn, wktGeomtryTypes[igtype], srsid, zflag, mflag);
    sqlite3_exec_free(context, db, sql, NULL);
}

// SQL function: GPKG_AddSpatialIndex(tableName, geometryColumn, idColumn); 
// Creates a spatial index of a table and the corresponding triggers to maintain the integrity between the spatial index and the table
// tableName -> Name of the table
// geometryColumn -> Column that contains the geometry
// idColumn -> Column that is the PrimaryKey of the table
// The spatial index created is called rtree_tableName_geometryColumn
// The triggers are called rtree_tableName_geometryColumn_insert, rtree_tableName_geometryColumn_update1, rtree_tableName_geometryColumn_update2, 
//                         rtree_tableName_geometryColumn_update5, rtree_tableName_geometryColumn_update4, rtree_tableName_geometryColumn_delete
// Registers the gpkg extension gpkg_rtree_index
// Populates the spatial index
// On success returns nothing. If there is an error throw an exception
static void fnct_GPKGAddSpatialIndex(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    const char* table;
    const char* gcolumn;
    const char* icolumn;
    sqlite3* db;
    char* sql, * errsql;

    // Get the parameters
    table = (const char*)sqlite3_value_text(argv[0]);
    gcolumn = (const char*)sqlite3_value_text(argv[1]);
    icolumn = (const char*)sqlite3_value_text(argv[2]);

    // Get DB handle
    db = sqlite3_context_db_handle(context);

    // Create rtree
    sql = sqlite3_mprintf("CREATE VIRTUAL TABLE \"rtree_%w_%w\" USING rtree(id, minx, maxx, miny, maxy)",
        table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, NULL) != SQLITE_OK)
        return;

    // Conditions: Insertion of non-empty geometry
    //    Actions: Insert record into rtree
    sql = sqlite3_mprintf("CREATE TRIGGER \"rtree_%w_%w_insert\" AFTER INSERT ON \"%w\" WHEN (NEW.\"%w\" NOT NULL AND NOT ST_IsEmpty(NEW.\"%w\"))\nBEGIN\n   INSERT OR REPLACE INTO \"rtree_%w_%w\" VALUES (NEW.\"%w\", ST_MinX(NEW.\"%w\"), ST_MaxX(NEW.\"%w\"), ST_MinY(NEW.\"%w\"), ST_MaxY(NEW.\"%w\"));\nEND;",
        table, gcolumn, table, gcolumn, gcolumn, table, gcolumn, icolumn, gcolumn, gcolumn, gcolumn, gcolumn);
    errsql = sqlite3_mprintf("DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Trigger update1 deprecated and replaced by update6 and update7


    // Conditions: Update of geometry column to empty geometry
    //             No row ID change
    //    Actions: Remove record from rtree
    sql = sqlite3_mprintf("CREATE TRIGGER \"rtree_%w_%w_update2\" AFTER UPDATE OF \"%w\" ON \"%w\" WHEN OLD.\"%w\" = NEW.\"%w\" AND (NEW.\"%w\" IS NULL OR ST_IsEmpty(NEW.\"%w\"))\nBEGIN\n   DELETE FROM \"rtree_%w_%w\" WHERE id = OLD.\"%w\";\nEND;",
        table, gcolumn, gcolumn, table, icolumn, icolumn, gcolumn, gcolumn, table, gcolumn, icolumn);
    errsql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Trigger update3 deprecated and replaced by update5

    // Conditions: Update of any column
    //             Row ID change
    //             Empty geometry
    //    Actions: Remove record from rtree for old and new <i>
    sql = sqlite3_mprintf("CREATE TRIGGER \"rtree_%w_%w_update4\" AFTER UPDATE ON \"%w\" WHEN OLD.\"%w\" != NEW.\"%w\" AND (NEW.\"%w\" IS NULL OR ST_IsEmpty(NEW.\"%w\"))\nBEGIN\n   DELETE FROM \"rtree_%w_%w\" WHERE id IN (OLD.\"%w\", NEW.\"%w\");\nEND;\n",
        table, gcolumn, table, icolumn, icolumn, gcolumn, gcolumn, table, gcolumn, icolumn, icolumn);
    errsql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Conditions: Update of any column
    //             Row ID change
    //             Non-empty geometry
    //    Actions: Remove record from rtree for old <i>
    //             Insert record into rtree for new <i>
    sql = sqlite3_mprintf("CREATE TRIGGER \"rtree_%w_%w_update5\" AFTER UPDATE ON \"%w\" WHEN OLD.\"%w\" != NEW.\"%w\" AND (NEW.\"%w\" NOT NULL AND NOT ST_IsEmpty(NEW.\"%w\"))\nBEGIN\n   DELETE FROM \"rtree_%w_%w\" WHERE id = OLD.\"%w\";\n   INSERT OR REPLACE INTO \"rtree_%w_%w\" VALUES (NEW.\"%w\", ST_MinX(NEW.\"%w\"), ST_MaxX(NEW.\"%w\"), ST_MinY(NEW.\"%w\"), ST_MaxY(NEW.\"%w\"));\nEND;",
        table, gcolumn, table, icolumn, icolumn, gcolumn, gcolumn, table, gcolumn, icolumn, table, gcolumn, icolumn, gcolumn, gcolumn, gcolumn, gcolumn);
    errsql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_update4\"; DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Conditions: Update a non-empty geometry with another non-empty geometry
    //    Actions: Replace record from rtree for <i>
    sql = sqlite3_mprintf("CREATE TRIGGER \"rtree_%w_%w_update6\" AFTER UPDATE OF \"%w\" ON \"%w\" WHEN OLD.\"%w\" = NEW.\"%w\" AND (NEW.\"%w\" NOT NULL AND NOT ST_IsEmpty(NEW.\"%w\")) AND (OLD.\"%w\" NOT NULL AND NOT ST_IsEmpty(OLD.\"%w\"))\nBEGIN\n   UPDATE \"rtree_%w_%w\" SET minx = ST_MinX(NEW.\"%w\"), maxx = ST_MaxX(NEW.\"%w\"), miny = ST_MinY(NEW.\"%w\"), maxy = ST_MaxY(NEW.\"%w\") where id = NEW.\"%w\";\nEND;",
        table, gcolumn, gcolumn, table, icolumn, icolumn, gcolumn, gcolumn, gcolumn, gcolumn, table, gcolumn, gcolumn, gcolumn, gcolumn, gcolumn, icolumn);
    errsql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_update5\"; DROP TRIGGER \"rtree_%w_%w_update4\"; DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Conditions: Update a null/empty geometry with a non-empty geometry
    //    Actions: Insert record into rtree for new <i>
    sql = sqlite3_mprintf("CREATE TRIGGER \"rtree_%w_%w_update7\" AFTER UPDATE OF \"%w\" ON \"%w\" WHEN OLD.\"%w\" = NEW.\"%w\" AND (NEW.\"%w\" NOT NULL AND NOT ST_IsEmpty(NEW.\"%w\")) AND (OLD.\"%w\" IS NULL OR ST_IsEmpty(OLD.\"%w\"))\nBEGIN\n   INSERT INTO \"rtree_%w_%w\" VALUES (NEW.\"%w\", ST_MinX(NEW.\"%w\"), ST_MaxX(NEW.\"%w\"), ST_MinY(NEW.\"%w\"), ST_MaxY(NEW.\"%w\"));\nEND;",
        table, gcolumn, gcolumn, table, icolumn, icolumn, gcolumn, gcolumn, gcolumn, gcolumn, table, gcolumn, icolumn, gcolumn, gcolumn, gcolumn, gcolumn);
    errsql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_update6\"; DROP TRIGGER \"rtree_%w_%w_update5\"; DROP TRIGGER \"rtree_%w_%w_update4\"; DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Conditions: Row deleted
    //    Actions: Remove record from rtree for old <i>
    sql = sqlite3_mprintf("CREATE TRIGGER \"rtree_%w_%w_delete\" AFTER DELETE ON \"%w\" WHEN old.\"%w\" NOT NULL\nBEGIN\n   DELETE FROM \"rtree_%w_%w\" WHERE id = OLD.\"%w\";\nEND;",
        table, gcolumn, table, gcolumn, table, gcolumn, icolumn);
    errsql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_update7\"; DROP TRIGGER \"rtree_%w_%w_update6\"; DROP TRIGGER \"rtree_%w_%w_update5\"; DROP TRIGGER \"rtree_%w_%w_update4\"; DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Register GPKG Extension
    sql = sqlite3_mprintf("INSERT INTO gpkg_extensions(table_name, column_name, extension_name, definition, scope)  VALUES(%Q, %Q, 'gpkg_rtree_index', 'http://www.geopackage.org', 'write-only')",
        table, gcolumn);
    errsql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_delete\"; DROP TRIGGER \"rtree_%w_%w_update7\"; DROP TRIGGER \"rtree_%w_%w_update6\"; DROP TRIGGER \"rtree_%w_%w_update5\"; DROP TRIGGER \"rtree_%w_%w_update4\"; DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // Populate rtree
    sql = sqlite3_mprintf("INSERT OR REPLACE INTO \"rtree_%w_%w\" SELECT \"%w\", ST_MinX(\"%w\"), ST_MaxX(\"%w\"), ST_MinY(\"%w\"), ST_MaxY(\"%w\") FROM \"%w\"",
        table, gcolumn, icolumn, gcolumn, gcolumn, gcolumn, gcolumn, table);
    errsql = sqlite3_mprintf("DELETE FROM gpkg_extensions where table_name = %Q AND column_name = %Q AND extension_name = 'gpkg_rtree_index'; DROP TRIGGER \"rtree_%w_%w_delete\"; DROP TRIGGER \"rtree_%w_%w_update7\"; DROP TRIGGER \"rtree_%w_%w_update6\"; DROP TRIGGER \"rtree_%w_%w_update5\"; DROP TRIGGER \"rtree_%w_%w_update4\"; DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;
}

// SQL function: GPKG_DropSpatialIndex(table, geometryColumn, idColumn); 
// tableName -> Name of the table
// geometryColumn -> Column that contains the geometry
// idColumn -> Column tha is the PrimaryKey of the table
// Drops a spatial index of a table and the corresponding triggers to maintain the integrity between the spatial index and the table
// Unregisters the gpkg extension gpkg_rtree_index
// On success returns nothing. If there is an error throw an exception
static void fnct_GPKGDropSpatialIndex(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    const char* table;
    const char* gcolumn;
    sqlite3* db;
    char* sql;

    // Get the parameters
    table = (const char*)sqlite3_value_text(argv[0]);
    gcolumn = (const char*)sqlite3_value_text(argv[1]);

    // Get DB handle
    db = sqlite3_context_db_handle(context);

    // Drop triggers and RTree
    sql = sqlite3_mprintf("DROP TRIGGER \"rtree_%w_%w_delete\"; DROP TRIGGER \"rtree_%w_%w_update7\"; DROP TRIGGER \"rtree_%w_%w_update6\"; DROP TRIGGER \"rtree_%w_%w_update5\"; DROP TRIGGER \"rtree_%w_%w_update4\"; DROP TRIGGER \"rtree_%w_%w_update2\"; DROP TRIGGER \"rtree_%w_%w_insert\"; DROP TABLE \"rtree_%w_%w\"",
        table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn, table, gcolumn);
    if (sqlite3_exec_free(context, db, sql, NULL) != SQLITE_OK)
        return;

    // Remove GPKG Extension
    sql = sqlite3_mprintf("DELETE FROM gpkg_extensions WHERE LOWER(table_name) = LOWER(%Q) AND LOWER(column_name) = LOWER(%Q) AND extension_name = 'gpkg_rtree_index'",
        table, gcolumn);
    sqlite3_exec_free(context, db, sql, NULL);
}

// SQL function: GPKG_ExtVersion(); 
// Returns an string showing the version of this extension
// On success returns nothing. If there is an error throw an exception
static void fnct_GPKGExtVersion(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    sqlite3_result_text(context, VERSION, -1, NULL);
}

// SQL function: GPKG_Version(); 
// Returns an integer showing the GeoPackage version
// Is the value stored as PRAGMA user_version
// On success returns nothing. If there is an error throw an exception
static void fnct_GPKGVersion(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    sqlite3* db;
    char* sql;
    sqlite3_stmt* stmt;
    int ret;
    int version = 0;

    // Get DB handle
    db = sqlite3_context_db_handle(context);

    // Query the version
    sql = sqlite3_mprintf("PRAGMA user_version");
    ret = sqlite3_prepare_v2(db, sql, (int)strlen(sql), &stmt, NULL);
    if (ret != SQLITE_OK)
    {
        sqlite3_result_error(context, "GPKG_Version() error", -1);
        return;
    }
    ret = sqlite3_step(stmt);
    if (ret == SQLITE_ROW)
        version = sqlite3_column_int(stmt, 0);
    else
    {
        sqlite3_result_error(context, "GPKG_Version() version undefined", -1);
        return;
    }
    sqlite3_finalize(stmt);

    // Return the result
    sqlite3_result_int(context, version);
}

// SQL function: GPKG_Initialize(version); 
// Creates the base tables for an empty GeoPackage
// version -> optional parameter must be 10200 or 10300 or 10400. If not specified assumes 10400.
// On success returns nothing. If there is an error throw an exception
static void fnct_GPKGInitialize(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    sqlite3* db;
    char* sql, * errsql;
    int userVersion = GPKG_VERSION_10400;

    // Get the parameters
    if (argc == 1)
    {
        userVersion = sqlite3_value_int(argv[0]);
        if (userVersion != GPKG_VERSION_10200 && userVersion != GPKG_VERSION_10300 && userVersion != GPKG_VERSION_10400)
        {
            sqlite3_result_error(context, "GPKG_Initialize() error: argument 1 [version] unsupported value. Must be 10200 or 10300 or 10400.", -1);
            return;
        }
    }

    // Get DB handle
    db = sqlite3_context_db_handle(context);

    // Set Application ID (Clause 1.1.1.1.1 Req 2)
    sql = sqlite3_mprintf("PRAGMA application_id = %d", GPKG_APPLICATION_ID);
    if (sqlite3_exec_free(context, db, sql, NULL) != SQLITE_OK)
        return;

    // Set User Version (Clause 1.1.1.1.1 Req 2)
    sql = sqlite3_mprintf("PRAGMA user_version = %d", userVersion);
    if (sqlite3_exec_free(context, db, sql, NULL) != SQLITE_OK)
        return;

    // C.1. gpkg_spatial_ref_sys
    sql = sqlite3_mprintf("CREATE TABLE gpkg_spatial_ref_sys(\n   srs_name TEXT NOT NULL,\n   srs_id INTEGER NOT NULL PRIMARY KEY,\n   organization TEXT NOT NULL,\n   organization_coordsys_id INTEGER NOT NULL,\n   definition TEXT NOT NULL,\n   description TEXT\n)");
    if (sqlite3_exec_free(context, db, sql, NULL) != SQLITE_OK)
        return;

    // Default gpkg_spatial_ref_sys minimum records
    sql = sqlite3_mprintf("INSERT INTO gpkg_spatial_ref_sys(srs_name, srs_id, organization, organization_coordsys_id, definition, description) VALUES('Undefined cartesian SRS', -1, 'NONE', -1, 'undefined', 'undefined cartesian coordinate reference system')");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;
    sql = sqlite3_mprintf("INSERT INTO gpkg_spatial_ref_sys(srs_name, srs_id, organization, organization_coordsys_id, definition, description) VALUES('Undefined geographic SRS ', 0, 'NONE', 0, 'undefined', 'undefined geographic coordinate reference system')");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;
    sql = sqlite3_mprintf("INSERT INTO gpkg_spatial_ref_sys(srs_name, srs_id, organization, organization_coordsys_id, definition, description) VALUES('WGS84', 4326, 'epsg', 4326, 'GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563,AUTHORITY[\"EPSG\",\"7030\"]],AUTHORITY[\"EPSG\",\"6326\"]],PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],AUTHORITY[\"EPSG\",\"4326\"]]', 'longitude/latitude coordinates in decimal degrees on the WGS 84 spheroid')");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // C.2. gpkg_contents
    sql = sqlite3_mprintf("CREATE TABLE gpkg_contents(\n   table_name TEXT NOT NULL PRIMARY KEY,\n   data_type TEXT NOT NULL,\n   identifier TEXT UNIQUE,\n   description TEXT DEFAULT '',\n   last_change DATETIME NOT NULL DEFAULT(strftime('%%Y-%%m-%%dT%%H:%%M:%%fZ', 'now')),\n   min_x DOUBLE,\n   min_y DOUBLE,\n   max_x DOUBLE,\n   max_y DOUBLE,\n   srs_id INTEGER,\n   CONSTRAINT fk_gc_r_srs_id FOREIGN KEY(srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)\n)");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // C.3. gpkg_geometry_columns
    sql = sqlite3_mprintf("CREATE TABLE gpkg_geometry_columns(\n   table_name TEXT NOT NULL,\n   column_name TEXT NOT NULL,\n   geometry_type_name TEXT NOT NULL,\n   srs_id INTEGER NOT NULL,\n   z TINYINT NOT NULL,\n   m TINYINT NOT NULL,\n   CONSTRAINT pk_geom_cols PRIMARY KEY(table_name, column_name),\n   CONSTRAINT fk_gc_tn FOREIGN KEY(table_name) REFERENCES gpkg_contents(table_name),\n   CONSTRAINT fk_gc_srs FOREIGN KEY(srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)\n)");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // C.5. gpkg_tile_matrix_set
    sql = sqlite3_mprintf("CREATE TABLE gpkg_tile_matrix_set(\n   table_name TEXT NOT NULL PRIMARY KEY,\n   srs_id INTEGER NOT NULL,\n   min_x DOUBLE NOT NULL,\n   min_y DOUBLE NOT NULL,\n   max_x DOUBLE NOT NULL,\n   max_y DOUBLE NOT NULL,\n   CONSTRAINT fk_gtms_table_name FOREIGN KEY(table_name) REFERENCES gpkg_contents(table_name),\n   CONSTRAINT fk_gtms_srs FOREIGN KEY(srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)\n)");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // C.6. gpkg_tile_matrix
    sql = sqlite3_mprintf("CREATE TABLE gpkg_tile_matrix(\n   table_name TEXT NOT NULL,\n   zoom_level INTEGER NOT NULL,\n   matrix_width INTEGER NOT NULL,\n   matrix_height INTEGER NOT NULL,\n   tile_width INTEGER NOT NULL,\n   tile_height INTEGER NOT NULL,\n   pixel_x_size DOUBLE NOT NULL,\n   pixel_y_size DOUBLE NOT NULL,\n   CONSTRAINT pk_ttm PRIMARY KEY(table_name, zoom_level),\n   CONSTRAINT fk_tmm_table_name FOREIGN KEY(table_name) REFERENCES gpkg_contents(table_name)\n)");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // D.1. gpkg_tile_matrix triggers
    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_zoom_level_insert' BEFORE INSERT ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'insert on table ''gpkg_tile_matrix'' violates constraint: zoom_level cannot be less than 0') WHERE (NEW.zoom_level < 0);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_zoom_level_update' BEFORE UPDATE of zoom_level ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'update on table ''gpkg_tile_matrix'' violates constraint: zoom_level cannot be less than 0') WHERE (NEW.zoom_level < 0);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_matrix_width_insert' BEFORE INSERT ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'insert on table ''gpkg_tile_matrix'' violates constraint: matrix_width cannot be less than 1') WHERE (NEW.matrix_width < 1);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_matrix_width_update' BEFORE UPDATE OF matrix_width ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'update on table ''gpkg_tile_matrix'' violates constraint: matrix_width cannot be less than 1') WHERE (NEW.matrix_width < 1);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_matrix_height_insert' BEFORE INSERT ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'insert on table ''gpkg_tile_matrix'' violates constraint: matrix_height cannot be less than 1') WHERE (NEW.matrix_height < 1);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_matrix_height_update' BEFORE UPDATE OF matrix_height ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'update on table ''gpkg_tile_matrix'' violates constraint: matrix_height cannot be less than 1') WHERE (NEW.matrix_height < 1);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_pixel_x_size_insert' BEFORE INSERT ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'insert on table ''gpkg_tile_matrix'' violates constraint: pixel_x_size must be greater than 0') WHERE NOT (NEW.pixel_x_size > 0);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_pixel_x_size_update' BEFORE UPDATE OF pixel_x_size ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'update on table ''gpkg_tile_matrix'' violates constraint: pixel_x_size must be greater than 0') WHERE NOT (NEW.pixel_x_size > 0);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_pixel_y_size_insert' BEFORE INSERT ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'insert on table ''gpkg_tile_matrix'' violates constraint: pixel_y_size must be greater than 0') WHERE NOT (NEW.pixel_y_size > 0);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    sql = sqlite3_mprintf("CREATE TRIGGER 'gpkg_tile_matrix_pixel_y_size_update' BEFORE UPDATE OF pixel_y_size ON 'gpkg_tile_matrix' FOR EACH ROW BEGIN\n   SELECT RAISE(ABORT, 'update on table ''gpkg_tile_matrix'' violates constraint: pixel_y_size must be greater than 0') WHERE NOT (NEW.pixel_y_size > 0);\nEND;");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;

    // C.8. gpkg_extensions
    sql = sqlite3_mprintf("CREATE TABLE gpkg_extensions(\n   table_name TEXT, column_name TEXT,\n   extension_name TEXT NOT NULL,\n   definition TEXT NOT NULL,\n   scope TEXT NOT NULL,\n   CONSTRAINT ge_tce UNIQUE(table_name, column_name, extension_name)\n)");
    errsql = sqlite3_mprintf("DROP TABLE gpkg_tile_matrix; DROP TABLE gpkg_tile_matrix_set; DROP TABLE gpkg_geometry_columns; DROP TABLE gpkg_contents; DROP TABLE gpkg_spatial_ref_sys");
    if (sqlite3_exec_free(context, db, sql, errsql) != SQLITE_OK)
        return;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_gpkg_init(sqlite3* db, char** pzErrMsg, const sqlite3_api_routines* pApi)
{
    int rc = SQLITE_OK;
    SQLITE_EXTENSION_INIT2(pApi);
    (void)pzErrMsg;  /* Unused parameter */

    sqlite3_create_function_v2(db, "ST_MinX", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMinX, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_MinY", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMinY, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_MinZ", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMinZ, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_MinM", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMinM, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_MaxX", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMaxX, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_MaxY", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMaxY, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_MaxZ", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMaxZ, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_MaxM", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STMaxM, 0, 0, 0);
    sqlite3_create_function_v2(db, "ST_IsEmpty", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_STIsEmpty, 0, 0, 0);

    sqlite3_create_function_v2(db, "GPKG_AddGeometryColumn", 7, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_GPKGAddGeometryColumn, 0, 0, 0);
    sqlite3_create_function_v2(db, "GPKG_AddSpatialIndex", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_GPKGAddSpatialIndex, 0, 0, 0);
    sqlite3_create_function_v2(db, "GPKG_DropSpatialIndex", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_GPKGDropSpatialIndex, 0, 0, 0);
    sqlite3_create_function_v2(db, "GPKG_ExtVersion", 0, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_GPKGExtVersion, 0, 0, 0);
    sqlite3_create_function_v2(db, "GPKG_Version", 0, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_GPKGVersion, 0, 0, 0);
    sqlite3_create_function_v2(db, "GPKG_Initialize", 0, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_GPKGInitialize, 0, 0, 0);
    sqlite3_create_function_v2(db, "GPKG_Initialize", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0, fnct_GPKGInitialize, 0, 0, 0);

    return rc;
}