/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2017, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file nodelist.c
 *
 * \brief Structures and functions for tracking what we know about the routers
 *   on the Tor network, and correlating information from networkstatus,
 *   routerinfo, and microdescs.
 *
 * The key structure here is node_t: that's the canonical way to refer
 * to a Tor relay that we might want to build a circuit through.  Every
 * node_t has either a routerinfo_t, or a routerstatus_t from the current
 * networkstatus consensus.  If it has a routerstatus_t, it will also
 * need to have a microdesc_t before you can use it for circuits.
 *
 * The nodelist_t is a global singleton that maps identities to node_t
 * objects.  Access them with the node_get_*() functions.  The nodelist_t
 * is maintained by calls throughout the codebase
 *
 * Generally, other code should not have to reach inside a node_t to
 * see what information it has.  Instead, you should call one of the
 * many accessor functions that works on a generic node_t.  If there
 * isn't one that does what you need, it's better to make such a function,
 * and then use it.
 *
 * For historical reasons, some of the functions that select a node_t
 * from the list of all usable node_t objects are in the routerlist.c
 * module, since they originally selected a routerinfo_t. (TODO: They
 * should move!)
 *
 * (TODO: Perhaps someday we should abstract the remaining ways of
 * talking about a relay to also be node_t instances. Those would be
 * routerstatus_t as used for directory requests, and dir_server_t as
 * used for authorities and fallback directories.)
 */

#include "or.h"
#include "address.h"
#include "config.h"
#include "control.h"
#include "dirserv.h"
#include "entrynodes.h"
#include "geoip.h"
#include "main.h"
#include "microdesc.h"
#include "networkstatus.h"
#include "nodelist.h"
#include "policies.h"
#include "protover.h"
#include "rendservice.h"
#include "router.h"
#include "routerlist.h"
#include "routerset.h"
#include "torcert.h"

#include <string.h>

static void nodelist_drop_node(node_t *node, int remove_from_ht);
static void node_free(node_t *node);

/** count_usable_descriptors counts descriptors with these flag(s)
 */
typedef enum {
  /* All descriptors regardless of flags */
  USABLE_DESCRIPTOR_ALL = 0,
  /* Only descriptors with the Exit flag */
  USABLE_DESCRIPTOR_EXIT_ONLY = 1
} usable_descriptor_t;
static void count_usable_descriptors(int *num_present,
                                     int *num_usable,
                                     smartlist_t *descs_out,
                                     const networkstatus_t *consensus,
                                     time_t now,
                                     routerset_t *in_set,
                                     usable_descriptor_t exit_only);
static void update_router_have_minimum_dir_info(void);
static double get_frac_paths_needed_for_circs(const or_options_t *options,
                                              const networkstatus_t *ns);

/** A nodelist_t holds a node_t object for every router we're "willing to use
 * for something".  Specifically, it should hold a node_t for every node that
 * is currently in the routerlist, or currently in the consensus we're using.
 */
typedef struct nodelist_t {
  /* A list of all the nodes. */
  smartlist_t *nodes;
  /* Hash table to map from node ID digest to node. */
  HT_HEAD(nodelist_map, node_t) nodes_by_id;

} nodelist_t;

static inline unsigned int
node_id_hash(const node_t *node)
{
  return (unsigned) siphash24g(node->identity, DIGEST_LEN);
}

static inline unsigned int
node_id_eq(const node_t *node1, const node_t *node2)
{
  return tor_memeq(node1->identity, node2->identity, DIGEST_LEN);
}

HT_PROTOTYPE(nodelist_map, node_t, ht_ent, node_id_hash, node_id_eq)
HT_GENERATE2(nodelist_map, node_t, ht_ent, node_id_hash, node_id_eq,
             0.6, tor_reallocarray_, tor_free_)

/** The global nodelist. */
static nodelist_t *the_nodelist=NULL;

/** Create an empty nodelist if we haven't done so already. */
static void
init_nodelist(void)
{
  if (PREDICT_UNLIKELY(the_nodelist == NULL)) {
    the_nodelist = tor_malloc_zero(sizeof(nodelist_t));
    HT_INIT(nodelist_map, &the_nodelist->nodes_by_id);
    the_nodelist->nodes = smartlist_new();
  }
}

/** As node_get_by_id, but returns a non-const pointer */
node_t *
node_get_mutable_by_id(const char *identity_digest)
{
  node_t search, *node;
  if (PREDICT_UNLIKELY(the_nodelist == NULL))
    return NULL;

  memcpy(&search.identity, identity_digest, DIGEST_LEN);
  node = HT_FIND(nodelist_map, &the_nodelist->nodes_by_id, &search);
  return node;
}

/** Return the node_t whose identity is <b>identity_digest</b>, or NULL
 * if no such node exists. */
MOCK_IMPL(const node_t *,
node_get_by_id,(const char *identity_digest))
{
  return node_get_mutable_by_id(identity_digest);
}

/** Internal: return the node_t whose identity_digest is
 * <b>identity_digest</b>.  If none exists, create a new one, add it to the
 * nodelist, and return it.
 *
 * Requires that the nodelist be initialized.
 */
static node_t *
node_get_or_create(const char *identity_digest)
{
  node_t *node;

  if ((node = node_get_mutable_by_id(identity_digest)))
    return node;

  node = tor_malloc_zero(sizeof(node_t));
  memcpy(node->identity, identity_digest, DIGEST_LEN);
  HT_INSERT(nodelist_map, &the_nodelist->nodes_by_id, node);

  smartlist_add(the_nodelist->nodes, node);
  node->nodelist_idx = smartlist_len(the_nodelist->nodes) - 1;

  node->country = -1;

  return node;
}

/** Called when a node's address changes. */
static void
node_addrs_changed(node_t *node)
{
  node->last_reachable = node->last_reachable6 = 0;
  node->country = -1;
}

/** Add <b>ri</b> to an appropriate node in the nodelist.  If we replace an
 * old routerinfo, and <b>ri_old_out</b> is not NULL, set *<b>ri_old_out</b>
 * to the previous routerinfo.
 */
node_t *
nodelist_set_routerinfo(routerinfo_t *ri, routerinfo_t **ri_old_out)
{
  node_t *node;
  const char *id_digest;
  int had_router = 0;
  tor_assert(ri);

  init_nodelist();
  id_digest = ri->cache_info.identity_digest;
  node = node_get_or_create(id_digest);

  if (node->ri) {
    if (!routers_have_same_or_addrs(node->ri, ri)) {
      node_addrs_changed(node);
    }
    had_router = 1;
    if (ri_old_out)
      *ri_old_out = node->ri;
  } else {
    if (ri_old_out)
      *ri_old_out = NULL;
  }
  node->ri = ri;

  if (node->country == -1)
    node_set_country(node);

  if (authdir_mode(get_options()) && !had_router) {
    const char *discard=NULL;
    uint32_t status = dirserv_router_get_status(ri, &discard, LOG_INFO);
    dirserv_set_node_flags_from_authoritative_status(node, status);
  }

  return node;
}

/** Set the appropriate node_t to use <b>md</b> as its microdescriptor.
 *
 * Called when a new microdesc has arrived and the usable consensus flavor
 * is "microdesc".
 **/
node_t *
nodelist_add_microdesc(microdesc_t *md)
{
  networkstatus_t *ns =
    networkstatus_get_latest_consensus_by_flavor(FLAV_MICRODESC);
  const routerstatus_t *rs;
  node_t *node;
  if (ns == NULL)
    return NULL;
  init_nodelist();

  /* Microdescriptors don't carry an identity digest, so we need to figure
   * it out by looking up the routerstatus. */
  rs = router_get_consensus_status_by_descriptor_digest(ns, md->digest);
  if (rs == NULL)
    return NULL;
  node = node_get_mutable_by_id(rs->identity_digest);
  if (node) {
    if (node->md)
      node->md->held_by_nodes--;
    node->md = md;
    md->held_by_nodes++;
  }
  return node;
}

/** Tell the nodelist that the current usable consensus is <b>ns</b>.
 * This makes the nodelist change all of the routerstatus entries for
 * the nodes, drop nodes that no longer have enough info to get used,
 * and grab microdescriptors into nodes as appropriate.
 */
void
nodelist_set_consensus(networkstatus_t *ns)
{
  const or_options_t *options = get_options();
  int authdir = authdir_mode_v3(options);

  init_nodelist();
  if (ns->flavor == FLAV_MICRODESC)
    (void) get_microdesc_cache(); /* Make sure it exists first. */

  SMARTLIST_FOREACH(the_nodelist->nodes, node_t *, node,
                    node->rs = NULL);

  SMARTLIST_FOREACH_BEGIN(ns->routerstatus_list, routerstatus_t *, rs) {
    node_t *node = node_get_or_create(rs->identity_digest);
    node->rs = rs;
    if (ns->flavor == FLAV_MICRODESC) {
      if (node->md == NULL ||
          tor_memneq(node->md->digest,rs->descriptor_digest,DIGEST256_LEN)) {
        if (node->md)
          node->md->held_by_nodes--;
        node->md = microdesc_cache_lookup_by_digest256(NULL,
                                                       rs->descriptor_digest);
        if (node->md)
          node->md->held_by_nodes++;
      }
    }

    node_set_country(node);

    /* If we're not an authdir, believe others. */
    if (!authdir) {
      node->is_valid = rs->is_valid;
      node->is_running = rs->is_flagged_running;
      node->is_fast = rs->is_fast;
      node->is_stable = rs->is_stable;
      node->is_possible_guard = rs->is_possible_guard;
      node->is_exit = rs->is_exit;
      node->is_bad_exit = rs->is_bad_exit;
      node->is_hs_dir = rs->is_hs_dir;
      node->ipv6_preferred = 0;
      if (fascist_firewall_prefer_ipv6_orport(options) &&
          (tor_addr_is_null(&rs->ipv6_addr) == 0 ||
           (node->md && tor_addr_is_null(&node->md->ipv6_addr) == 0)))
        node->ipv6_preferred = 1;
    }

  } SMARTLIST_FOREACH_END(rs);

  nodelist_purge();

  if (! authdir) {
    SMARTLIST_FOREACH_BEGIN(the_nodelist->nodes, node_t *, node) {
      /* We have no routerstatus for this router. Clear flags so we can skip
       * it, maybe.*/
      if (!node->rs) {
        tor_assert(node->ri); /* if it had only an md, or nothing, purge
                               * would have removed it. */
        if (node->ri->purpose == ROUTER_PURPOSE_GENERAL) {
          /* Clear all flags. */
          node->is_valid = node->is_running = node->is_hs_dir =
            node->is_fast = node->is_stable =
            node->is_possible_guard = node->is_exit =
            node->is_bad_exit = node->ipv6_preferred = 0;
        }
      }
    } SMARTLIST_FOREACH_END(node);
  }
}

/** Helper: return true iff a node has a usable amount of information*/
static inline int
node_is_usable(const node_t *node)
{
  return (node->rs) || (node->ri);
}

/** Tell the nodelist that <b>md</b> is no longer a microdescriptor for the
 * node with <b>identity_digest</b>. */
void
nodelist_remove_microdesc(const char *identity_digest, microdesc_t *md)
{
  node_t *node = node_get_mutable_by_id(identity_digest);
  if (node && node->md == md) {
    node->md = NULL;
    md->held_by_nodes--;
  }
}

/** Tell the nodelist that <b>ri</b> is no longer in the routerlist. */
void
nodelist_remove_routerinfo(routerinfo_t *ri)
{
  node_t *node = node_get_mutable_by_id(ri->cache_info.identity_digest);
  if (node && node->ri == ri) {
    node->ri = NULL;
    if (! node_is_usable(node)) {
      nodelist_drop_node(node, 1);
      node_free(node);
    }
  }
}

/** Remove <b>node</b> from the nodelist.  (Asserts that it was there to begin
 * with.) */
static void
nodelist_drop_node(node_t *node, int remove_from_ht)
{
  node_t *tmp;
  int idx;
  if (remove_from_ht) {
    tmp = HT_REMOVE(nodelist_map, &the_nodelist->nodes_by_id, node);
    tor_assert(tmp == node);
  }

  idx = node->nodelist_idx;
  tor_assert(idx >= 0);

  tor_assert(node == smartlist_get(the_nodelist->nodes, idx));
  smartlist_del(the_nodelist->nodes, idx);
  if (idx < smartlist_len(the_nodelist->nodes)) {
    tmp = smartlist_get(the_nodelist->nodes, idx);
    tmp->nodelist_idx = idx;
  }
  node->nodelist_idx = -1;
}

/** Return a newly allocated smartlist of the nodes that have <b>md</b> as
 * their microdescriptor. */
smartlist_t *
nodelist_find_nodes_with_microdesc(const microdesc_t *md)
{
  smartlist_t *result = smartlist_new();

  if (the_nodelist == NULL)
    return result;

  SMARTLIST_FOREACH_BEGIN(the_nodelist->nodes, node_t *, node) {
    if (node->md == md) {
      smartlist_add(result, node);
    }
  } SMARTLIST_FOREACH_END(node);

  return result;
}

/** Release storage held by <b>node</b>  */
static void
node_free(node_t *node)
{
  if (!node)
    return;
  if (node->md)
    node->md->held_by_nodes--;
  tor_assert(node->nodelist_idx == -1);
  tor_free(node);
}

/** Remove all entries from the nodelist that don't have enough info to be
 * usable for anything. */
void
nodelist_purge(void)
{
  node_t **iter;
  if (PREDICT_UNLIKELY(the_nodelist == NULL))
    return;

  /* Remove the non-usable nodes. */
  for (iter = HT_START(nodelist_map, &the_nodelist->nodes_by_id); iter; ) {
    node_t *node = *iter;

    if (node->md && !node->rs) {
      /* An md is only useful if there is an rs. */
      node->md->held_by_nodes--;
      node->md = NULL;
    }

    if (node_is_usable(node)) {
      iter = HT_NEXT(nodelist_map, &the_nodelist->nodes_by_id, iter);
    } else {
      iter = HT_NEXT_RMV(nodelist_map, &the_nodelist->nodes_by_id, iter);
      nodelist_drop_node(node, 0);
      node_free(node);
    }
  }
  nodelist_assert_ok();
}

/** Release all storage held by the nodelist. */
void
nodelist_free_all(void)
{
  if (PREDICT_UNLIKELY(the_nodelist == NULL))
    return;

  HT_CLEAR(nodelist_map, &the_nodelist->nodes_by_id);
  SMARTLIST_FOREACH_BEGIN(the_nodelist->nodes, node_t *, node) {
    node->nodelist_idx = -1;
    node_free(node);
  } SMARTLIST_FOREACH_END(node);

  smartlist_free(the_nodelist->nodes);

  tor_free(the_nodelist);
}

/** Check that the nodelist is internally consistent, and consistent with
 * the directory info it's derived from.
 */
void
nodelist_assert_ok(void)
{
  routerlist_t *rl = router_get_routerlist();
  networkstatus_t *ns = networkstatus_get_latest_consensus();
  digestmap_t *dm;

  if (!the_nodelist)
    return;

  dm = digestmap_new();

  /* every routerinfo in rl->routers should be in the nodelist. */
  if (rl) {
    SMARTLIST_FOREACH_BEGIN(rl->routers, routerinfo_t *, ri) {
      const node_t *node = node_get_by_id(ri->cache_info.identity_digest);
      tor_assert(node && node->ri == ri);
      tor_assert(fast_memeq(ri->cache_info.identity_digest,
                             node->identity, DIGEST_LEN));
      tor_assert(! digestmap_get(dm, node->identity));
      digestmap_set(dm, node->identity, (void*)node);
    } SMARTLIST_FOREACH_END(ri);
  }

  /* every routerstatus in ns should be in the nodelist */
  if (ns) {
    SMARTLIST_FOREACH_BEGIN(ns->routerstatus_list, routerstatus_t *, rs) {
      const node_t *node = node_get_by_id(rs->identity_digest);
      tor_assert(node && node->rs == rs);
      tor_assert(fast_memeq(rs->identity_digest, node->identity, DIGEST_LEN));
      digestmap_set(dm, node->identity, (void*)node);
      if (ns->flavor == FLAV_MICRODESC) {
        /* If it's a microdesc consensus, every entry that has a
         * microdescriptor should be in the nodelist.
         */
        microdesc_t *md =
          microdesc_cache_lookup_by_digest256(NULL, rs->descriptor_digest);
        tor_assert(md == node->md);
        if (md)
          tor_assert(md->held_by_nodes >= 1);
      }
    } SMARTLIST_FOREACH_END(rs);
  }

  /* The nodelist should have no other entries, and its entries should be
   * well-formed. */
  SMARTLIST_FOREACH_BEGIN(the_nodelist->nodes, node_t *, node) {
    tor_assert(digestmap_get(dm, node->identity) != NULL);
    tor_assert(node_sl_idx == node->nodelist_idx);
  } SMARTLIST_FOREACH_END(node);

  tor_assert((long)smartlist_len(the_nodelist->nodes) ==
             (long)HT_SIZE(&the_nodelist->nodes_by_id));

  digestmap_free(dm, NULL);
}

/** Return a list of a node_t * for every node we know about.  The caller
 * MUST NOT modify the list. (You can set and clear flags in the nodes if
 * you must, but you must not add or remove nodes.) */
MOCK_IMPL(smartlist_t *,
nodelist_get_list,(void))
{
  init_nodelist();
  return the_nodelist->nodes;
}

/** Given a hex-encoded nickname of the format DIGEST, $DIGEST, $DIGEST=name,
 * or $DIGEST~name, return the node with the matching identity digest and
 * nickname (if any).  Return NULL if no such node exists, or if <b>hex_id</b>
 * is not well-formed. */
const node_t *
node_get_by_hex_id(const char *hex_id)
{
  char digest_buf[DIGEST_LEN];
  char nn_buf[MAX_NICKNAME_LEN+1];
  char nn_char='\0';

  if (hex_digest_nickname_decode(hex_id, digest_buf, &nn_char, nn_buf)==0) {
    const node_t *node = node_get_by_id(digest_buf);
    if (!node)
      return NULL;
    if (nn_char) {
      const char *real_name = node_get_nickname(node);
      if (!real_name || strcasecmp(real_name, nn_buf))
        return NULL;
      if (nn_char == '=') {
        const char *named_id =
          networkstatus_get_router_digest_by_nickname(nn_buf);
        if (!named_id || tor_memneq(named_id, digest_buf, DIGEST_LEN))
          return NULL;
      }
    }
    return node;
  }

  return NULL;
}

/** Given a nickname (possibly verbose, possibly a hexadecimal digest), return
 * the corresponding node_t, or NULL if none exists.  Warn the user if
 * <b>warn_if_unnamed</b> is set, and they have specified a router by
 * nickname, but the Named flag isn't set for that router. */
MOCK_IMPL(const node_t *,
node_get_by_nickname,(const char *nickname, int warn_if_unnamed))
{
  if (!the_nodelist)
    return NULL;

  /* Handle these cases: DIGEST, $DIGEST, $DIGEST=name, $DIGEST~name. */
  {
    const node_t *node;
    if ((node = node_get_by_hex_id(nickname)) != NULL)
      return node;
  }

  if (!strcasecmp(nickname, UNNAMED_ROUTER_NICKNAME))
    return NULL;

  /* Okay, so if we get here, the nickname is just a nickname.  Is there
   * a binding for it in the consensus? */
  {
    const char *named_id =
      networkstatus_get_router_digest_by_nickname(nickname);
    if (named_id)
      return node_get_by_id(named_id);
  }

  /* Is it marked as owned-by-someone-else? */
  if (networkstatus_nickname_is_unnamed(nickname)) {
    log_info(LD_GENERAL, "The name %s is listed as Unnamed: there is some "
             "router that holds it, but not one listed in the current "
             "consensus.", escaped(nickname));
    return NULL;
  }

  /* Okay, so the name is not canonical for anybody. */
  {
    smartlist_t *matches = smartlist_new();
    const node_t *choice = NULL;

    SMARTLIST_FOREACH_BEGIN(the_nodelist->nodes, node_t *, node) {
      if (!strcasecmp(node_get_nickname(node), nickname))
        smartlist_add(matches, node);
    } SMARTLIST_FOREACH_END(node);

    if (smartlist_len(matches)>1 && warn_if_unnamed) {
      int any_unwarned = 0;
      SMARTLIST_FOREACH_BEGIN(matches, node_t *, node) {
        if (!node->name_lookup_warned) {
          node->name_lookup_warned = 1;
          any_unwarned = 1;
        }
      } SMARTLIST_FOREACH_END(node);

      if (any_unwarned) {
        log_warn(LD_CONFIG, "There are multiple matches for the name %s, "
                 "but none is listed as Named in the directory consensus. "
                 "Choosing one arbitrarily.", nickname);
      }
    } else if (smartlist_len(matches)==1 && warn_if_unnamed) {
      char fp[HEX_DIGEST_LEN+1];
      node_t *node = smartlist_get(matches, 0);
      if (! node->name_lookup_warned) {
        base16_encode(fp, sizeof(fp), node->identity, DIGEST_LEN);
        log_warn(LD_CONFIG,
                 "You specified a server \"%s\" by name, but the directory "
                 "authorities do not have any key registered for this "
                 "nickname -- so it could be used by any server, not just "
                 "the one you meant. "
                 "To make sure you get the same server in the future, refer "
                 "to it by key, as \"$%s\".", nickname, fp);
        node->name_lookup_warned = 1;
      }
    }

    if (smartlist_len(matches))
      choice = smartlist_get(matches, 0);

    smartlist_free(matches);
    return choice;
  }
}

/** Return the Ed25519 identity key for the provided node, or NULL if it
 * doesn't have one. */
const ed25519_public_key_t *
node_get_ed25519_id(const node_t *node)
{
  if (node->ri) {
    if (node->ri->cache_info.signing_key_cert) {
      const ed25519_public_key_t *pk =
        &node->ri->cache_info.signing_key_cert->signing_key;
      if (BUG(ed25519_public_key_is_zero(pk)))
        goto try_the_md;
      return pk;
    }
  }
 try_the_md:
  if (node->md) {
    if (node->md->ed25519_identity_pkey) {
      return node->md->ed25519_identity_pkey;
    }
  }
  return NULL;
}

/** Return true iff this node's Ed25519 identity matches <b>id</b>.
 * (An absent Ed25519 identity matches NULL or zero.) */
int
node_ed25519_id_matches(const node_t *node, const ed25519_public_key_t *id)
{
  const ed25519_public_key_t *node_id = node_get_ed25519_id(node);
  if (node_id == NULL || ed25519_public_key_is_zero(node_id)) {
    return id == NULL || ed25519_public_key_is_zero(id);
  } else {
    return id && ed25519_pubkey_eq(node_id, id);
  }
}

/** Return true iff <b>node</b> supports authenticating itself
 * by ed25519 ID during the link handshake in a way that we can understand
 * when we probe it. */
int
node_supports_ed25519_link_authentication(const node_t *node)
{
  /* XXXX Oh hm. What if some day in the future there are link handshake
   * versions that aren't 3 but which are ed25519 */
  if (! node_get_ed25519_id(node))
    return 0;
  if (node->ri) {
    const char *protos = node->ri->protocol_list;
    if (protos == NULL)
      return 0;
    return protocol_list_supports_protocol(protos, PRT_LINKAUTH, 3);
  }
  if (node->rs) {
    return node->rs->supports_ed25519_link_handshake;
  }
  tor_assert_nonfatal_unreached_once();
  return 0;
}

/** Return the RSA ID key's SHA1 digest for the provided node. */
const uint8_t *
node_get_rsa_id_digest(const node_t *node)
{
  tor_assert(node);
  return (const uint8_t*)node->identity;
}

/** Return the nickname of <b>node</b>, or NULL if we can't find one. */
const char *
node_get_nickname(const node_t *node)
{
  tor_assert(node);
  if (node->rs)
    return node->rs->nickname;
  else if (node->ri)
    return node->ri->nickname;
  else
    return NULL;
}

/** Return true iff the nickname of <b>node</b> is canonical, based on the
 * latest consensus. */
int
node_is_named(const node_t *node)
{
  const char *named_id;
  const char *nickname = node_get_nickname(node);
  if (!nickname)
    return 0;
  named_id = networkstatus_get_router_digest_by_nickname(nickname);
  if (!named_id)
    return 0;
  return tor_memeq(named_id, node->identity, DIGEST_LEN);
}

/** Return true iff <b>node</b> appears to be a directory authority or
 * directory cache */
int
node_is_dir(const node_t *node)
{
  if (node->rs) {
    routerstatus_t * rs = node->rs;
    /* This is true if supports_tunnelled_dir_requests is true which
     * indicates that we support directory request tunnelled or through the
     * DirPort. */
    return rs->is_v2_dir;
  } else if (node->ri) {
    routerinfo_t * ri = node->ri;
    /* Both tunnelled request is supported or DirPort is set. */
    return ri->supports_tunnelled_dir_requests;
  } else {
    return 0;
  }
}

/** Return true iff <b>node</b> has either kind of usable descriptor -- that
 * is, a routerdescriptor or a microdescriptor. */
int
node_has_descriptor(const node_t *node)
{
  return (node->ri ||
          (node->rs && node->md));
}

/** Return the router_purpose of <b>node</b>. */
int
node_get_purpose(const node_t *node)
{
  if (node->ri)
    return node->ri->purpose;
  else
    return ROUTER_PURPOSE_GENERAL;
}

/** Compute the verbose ("extended") nickname of <b>node</b> and store it
 * into the MAX_VERBOSE_NICKNAME_LEN+1 character buffer at
 * <b>verbose_name_out</b> */
void
node_get_verbose_nickname(const node_t *node,
                          char *verbose_name_out)
{
  const char *nickname = node_get_nickname(node);
  int is_named = node_is_named(node);
  verbose_name_out[0] = '$';
  base16_encode(verbose_name_out+1, HEX_DIGEST_LEN+1, node->identity,
                DIGEST_LEN);
  if (!nickname)
    return;
  verbose_name_out[1+HEX_DIGEST_LEN] = is_named ? '=' : '~';
  strlcpy(verbose_name_out+1+HEX_DIGEST_LEN+1, nickname, MAX_NICKNAME_LEN+1);
}

/** Compute the verbose ("extended") nickname of node with
 * given <b>id_digest</b> and store it into the MAX_VERBOSE_NICKNAME_LEN+1
 * character buffer at <b>verbose_name_out</b>
 *
 * If node_get_by_id() returns NULL, base 16 encoding of
 * <b>id_digest</b> is returned instead. */
void
node_get_verbose_nickname_by_id(const char *id_digest,
                                char *verbose_name_out)
{
  const node_t *node = node_get_by_id(id_digest);
  if (!node) {
    verbose_name_out[0] = '$';
    base16_encode(verbose_name_out+1, HEX_DIGEST_LEN+1, id_digest, DIGEST_LEN);
  } else {
    node_get_verbose_nickname(node, verbose_name_out);
  }
}

/** Return true iff it seems that <b>node</b> allows circuits to exit
 * through it directlry from the client. */
int
node_allows_single_hop_exits(const node_t *node)
{
  if (node && node->ri)
    return node->ri->allow_single_hop_exits;
  else
    return 0;
}

/** Return true iff it seems that <b>node</b> has an exit policy that doesn't
 * actually permit anything to exit, or we don't know its exit policy */
int
node_exit_policy_rejects_all(const node_t *node)
{
  if (node->rejects_all)
    return 1;

  if (node->ri)
    return node->ri->policy_is_reject_star;
  else if (node->md)
    return node->md->exit_policy == NULL ||
      short_policy_is_reject_star(node->md->exit_policy);
  else
    return 1;
}

/** Return true iff the exit policy for <b>node</b> is such that we can treat
 * rejecting an address of type <b>family</b> unexpectedly as a sign of that
 * node's failure. */
int
node_exit_policy_is_exact(const node_t *node, sa_family_t family)
{
  if (family == AF_UNSPEC) {
    return 1; /* Rejecting an address but not telling us what address
               * is a bad sign. */
  } else if (family == AF_INET) {
    return node->ri != NULL;
  } else if (family == AF_INET6) {
    return 0;
  }
  tor_fragile_assert();
  return 1;
}

/* Check if the "addr" and port_field fields from r are a valid non-listening
 * address/port. If so, set valid to true and add a newly allocated
 * tor_addr_port_t containing "addr" and port_field to sl.
 * "addr" is an IPv4 host-order address and port_field is a uint16_t.
 * r is typically a routerinfo_t or routerstatus_t.
 */
#define SL_ADD_NEW_IPV4_AP(r, port_field, sl, valid) \
  STMT_BEGIN \
    if (tor_addr_port_is_valid_ipv4h((r)->addr, (r)->port_field, 0)) { \
      valid = 1; \
      tor_addr_port_t *ap = tor_malloc(sizeof(tor_addr_port_t)); \
      tor_addr_from_ipv4h(&ap->addr, (r)->addr); \
      ap->port = (r)->port_field; \
      smartlist_add((sl), ap); \
    } \
  STMT_END

/* Check if the "addr" and port_field fields from r are a valid non-listening
 * address/port. If so, set valid to true and add a newly allocated
 * tor_addr_port_t containing "addr" and port_field to sl.
 * "addr" is a tor_addr_t and port_field is a uint16_t.
 * r is typically a routerinfo_t or routerstatus_t.
 */
#define SL_ADD_NEW_IPV6_AP(r, port_field, sl, valid) \
  STMT_BEGIN \
    if (tor_addr_port_is_valid(&(r)->ipv6_addr, (r)->port_field, 0)) { \
      valid = 1; \
      tor_addr_port_t *ap = tor_malloc(sizeof(tor_addr_port_t)); \
      tor_addr_copy(&ap->addr, &(r)->ipv6_addr); \
      ap->port = (r)->port_field; \
      smartlist_add((sl), ap); \
    } \
  STMT_END

/** Return list of tor_addr_port_t with all OR ports (in the sense IP
 * addr + TCP port) for <b>node</b>.  Caller must free all elements
 * using tor_free() and free the list using smartlist_free().
 *
 * XXX this is potentially a memory fragmentation hog -- if on
 * critical path consider the option of having the caller allocate the
 * memory
 */
smartlist_t *
node_get_all_orports(const node_t *node)
{
  smartlist_t *sl = smartlist_new();
  int valid = 0;

  /* Find a valid IPv4 address and port */
  if (node->ri != NULL) {
    SL_ADD_NEW_IPV4_AP(node->ri, or_port, sl, valid);
  }

  /* If we didn't find a valid address/port in the ri, try the rs */
  if (!valid && node->rs != NULL) {
    SL_ADD_NEW_IPV4_AP(node->rs, or_port, sl, valid);
  }

  /* Find a valid IPv6 address and port */
  valid = 0;
  if (node->ri != NULL) {
    SL_ADD_NEW_IPV6_AP(node->ri, ipv6_orport, sl, valid);
  }

  if (!valid && node->rs != NULL) {
    SL_ADD_NEW_IPV6_AP(node->rs, ipv6_orport, sl, valid);
  }

  if (!valid && node->md != NULL) {
    SL_ADD_NEW_IPV6_AP(node->md, ipv6_orport, sl, valid);
  }

  return sl;
}

#undef SL_ADD_NEW_IPV4_AP
#undef SL_ADD_NEW_IPV6_AP

/** Wrapper around node_get_prim_orport for backward
    compatibility.  */
void
node_get_addr(const node_t *node, tor_addr_t *addr_out)
{
  tor_addr_port_t ap;
  node_get_prim_orport(node, &ap);
  tor_addr_copy(addr_out, &ap.addr);
}

/** Return the host-order IPv4 address for <b>node</b>, or 0 if it doesn't
 * seem to have one.  */
uint32_t
node_get_prim_addr_ipv4h(const node_t *node)
{
  /* Don't check the ORPort or DirPort, as this function isn't port-specific,
   * and the node might have a valid IPv4 address, yet have a zero
   * ORPort or DirPort.
   */
  if (node->ri && tor_addr_is_valid_ipv4h(node->ri->addr, 0)) {
    return node->ri->addr;
  } else if (node->rs && tor_addr_is_valid_ipv4h(node->rs->addr, 0)) {
    return node->rs->addr;
  }
  return 0;
}

/** Copy a string representation of an IP address for <b>node</b> into
 * the <b>len</b>-byte buffer at <b>buf</b>.  */
void
node_get_address_string(const node_t *node, char *buf, size_t len)
{
  uint32_t ipv4_addr = node_get_prim_addr_ipv4h(node);

  if (tor_addr_is_valid_ipv4h(ipv4_addr, 0)) {
    tor_addr_t addr;
    tor_addr_from_ipv4h(&addr, ipv4_addr);
    tor_addr_to_str(buf, &addr, len, 0);
  } else if (len > 0) {
    buf[0] = '\0';
  }
}

/** Return <b>node</b>'s declared uptime, or -1 if it doesn't seem to have
 * one. */
long
node_get_declared_uptime(const node_t *node)
{
  if (node->ri)
    return node->ri->uptime;
  else
    return -1;
}

/** Return <b>node</b>'s platform string, or NULL if we don't know it. */
const char *
node_get_platform(const node_t *node)
{
  /* If we wanted, we could record the version in the routerstatus_t, since
   * the consensus lists it.  We don't, though, so this function just won't
   * work with microdescriptors. */
  if (node->ri)
    return node->ri->platform;
  else
    return NULL;
}

/** Return true iff <b>node</b> is one representing this router. */
int
node_is_me(const node_t *node)
{
  return router_digest_is_me(node->identity);
}

/** Return <b>node</b> declared family (as a list of names), or NULL if
 * the node didn't declare a family. */
const smartlist_t *
node_get_declared_family(const node_t *node)
{
  if (node->ri && node->ri->declared_family)
    return node->ri->declared_family;
  else if (node->md && node->md->family)
    return node->md->family;
  else
    return NULL;
}

/* Does this node have a valid IPv6 address?
 * Prefer node_has_ipv6_orport() or node_has_ipv6_dirport() for
 * checking specific ports. */
int
node_has_ipv6_addr(const node_t *node)
{
  /* Don't check the ORPort or DirPort, as this function isn't port-specific,
   * and the node might have a valid IPv6 address, yet have a zero
   * ORPort or DirPort.
   */
  if (node->ri && tor_addr_is_valid(&node->ri->ipv6_addr, 0))
    return 1;
  if (node->rs && tor_addr_is_valid(&node->rs->ipv6_addr, 0))
    return 1;
  if (node->md && tor_addr_is_valid(&node->md->ipv6_addr, 0))
    return 1;

  return 0;
}

/* Does this node have a valid IPv6 ORPort? */
int
node_has_ipv6_orport(const node_t *node)
{
  tor_addr_port_t ipv6_orport;
  node_get_pref_ipv6_orport(node, &ipv6_orport);
  return tor_addr_port_is_valid_ap(&ipv6_orport, 0);
}

/* Does this node have a valid IPv6 DirPort? */
int
node_has_ipv6_dirport(const node_t *node)
{
  tor_addr_port_t ipv6_dirport;
  node_get_pref_ipv6_dirport(node, &ipv6_dirport);
  return tor_addr_port_is_valid_ap(&ipv6_dirport, 0);
}

/** Return 1 if we prefer the IPv6 address and OR TCP port of
 * <b>node</b>, else 0.
 *
 *  We prefer the IPv6 address if the router has an IPv6 address,
 *  and we can use IPv6 addresses, and:
 *  i) the node_t says that it prefers IPv6
 *  or
 *  ii) the router has no IPv4 OR address.
 *
 * If you don't have a node, consider looking it up.
 * If there is no node, use fascist_firewall_prefer_ipv6_orport().
 */
int
node_ipv6_or_preferred(const node_t *node)
{
  const or_options_t *options = get_options();
  tor_addr_port_t ipv4_addr;
  node_assert_ok(node);

  /* XX/teor - node->ipv6_preferred is set from
   * fascist_firewall_prefer_ipv6_orport() each time the consensus is loaded.
   */
  if (!fascist_firewall_use_ipv6(options)) {
    return 0;
  } else if (node->ipv6_preferred || node_get_prim_orport(node, &ipv4_addr)) {
    return node_has_ipv6_orport(node);
  }
  return 0;
}

#define RETURN_IPV4_AP(r, port_field, ap_out) \
  STMT_BEGIN \
    if (r && tor_addr_port_is_valid_ipv4h((r)->addr, (r)->port_field, 0)) { \
      tor_addr_from_ipv4h(&(ap_out)->addr, (r)->addr); \
      (ap_out)->port = (r)->port_field; \
      return 0; \
    } \
  STMT_END

/** Copy the primary (IPv4) OR port (IP address and TCP port) for
 * <b>node</b> into *<b>ap_out</b>. Return 0 if a valid address and
 * port was copied, else return non-zero.*/
int
node_get_prim_orport(const node_t *node, tor_addr_port_t *ap_out)
{
  node_assert_ok(node);
  tor_assert(ap_out);

  /* Clear the address, as a safety precaution if calling functions ignore the
   * return value */
  tor_addr_make_null(&ap_out->addr, AF_INET);
  ap_out->port = 0;

  /* Check ri first, because rewrite_node_address_for_bridge() updates
   * node->ri with the configured bridge address. */

  RETURN_IPV4_AP(node->ri, or_port, ap_out);
  RETURN_IPV4_AP(node->rs, or_port, ap_out);
  /* Microdescriptors only have an IPv6 address */

  return -1;
}

/** Copy the preferred OR port (IP address and TCP port) for
 * <b>node</b> into *<b>ap_out</b>.  */
void
node_get_pref_orport(const node_t *node, tor_addr_port_t *ap_out)
{
  tor_assert(ap_out);

  if (node_ipv6_or_preferred(node)) {
    node_get_pref_ipv6_orport(node, ap_out);
  } else {
    /* the primary ORPort is always on IPv4 */
    node_get_prim_orport(node, ap_out);
  }
}

/** Copy the preferred IPv6 OR port (IP address and TCP port) for
 * <b>node</b> into *<b>ap_out</b>. */
void
node_get_pref_ipv6_orport(const node_t *node, tor_addr_port_t *ap_out)
{
  node_assert_ok(node);
  tor_assert(ap_out);

  /* Check ri first, because rewrite_node_address_for_bridge() updates
   * node->ri with the configured bridge address.
   * Prefer rs over md for consistency with the fascist_firewall_* functions.
   * Check if the address or port are valid, and try another alternative
   * if they are not. */

  if (node->ri && tor_addr_port_is_valid(&node->ri->ipv6_addr,
                                         node->ri->ipv6_orport, 0)) {
    tor_addr_copy(&ap_out->addr, &node->ri->ipv6_addr);
    ap_out->port = node->ri->ipv6_orport;
  } else if (node->rs && tor_addr_port_is_valid(&node->rs->ipv6_addr,
                                                 node->rs->ipv6_orport, 0)) {
    tor_addr_copy(&ap_out->addr, &node->rs->ipv6_addr);
    ap_out->port = node->rs->ipv6_orport;
  } else if (node->md && tor_addr_port_is_valid(&node->md->ipv6_addr,
                                                 node->md->ipv6_orport, 0)) {
    tor_addr_copy(&ap_out->addr, &node->md->ipv6_addr);
    ap_out->port = node->md->ipv6_orport;
  } else {
    tor_addr_make_null(&ap_out->addr, AF_INET6);
    ap_out->port = 0;
  }
}

/** Return 1 if we prefer the IPv6 address and Dir TCP port of
 * <b>node</b>, else 0.
 *
 *  We prefer the IPv6 address if the router has an IPv6 address,
 *  and we can use IPv6 addresses, and:
 *  i) the router has no IPv4 Dir address.
 *  or
 *  ii) our preference is for IPv6 Dir addresses.
 *
 * If there is no node, use fascist_firewall_prefer_ipv6_dirport().
 */
int
node_ipv6_dir_preferred(const node_t *node)
{
  const or_options_t *options = get_options();
  tor_addr_port_t ipv4_addr;
  node_assert_ok(node);

  /* node->ipv6_preferred is set from fascist_firewall_prefer_ipv6_orport(),
   * so we can't use it to determine DirPort IPv6 preference.
   * This means that bridge clients will use IPv4 DirPorts by default.
   */
  if (!fascist_firewall_use_ipv6(options)) {
    return 0;
  } else if (node_get_prim_dirport(node, &ipv4_addr)
      || fascist_firewall_prefer_ipv6_dirport(get_options())) {
    return node_has_ipv6_dirport(node);
  }
  return 0;
}

/** Copy the primary (IPv4) Dir port (IP address and TCP port) for
 * <b>node</b> into *<b>ap_out</b>. Return 0 if a valid address and
 * port was copied, else return non-zero.*/
int
node_get_prim_dirport(const node_t *node, tor_addr_port_t *ap_out)
{
  node_assert_ok(node);
  tor_assert(ap_out);

  /* Check ri first, because rewrite_node_address_for_bridge() updates
   * node->ri with the configured bridge address. */

  RETURN_IPV4_AP(node->ri, dir_port, ap_out);
  RETURN_IPV4_AP(node->rs, dir_port, ap_out);
  /* Microdescriptors only have an IPv6 address */

  return -1;
}

#undef RETURN_IPV4_AP

/** Copy the preferred Dir port (IP address and TCP port) for
 * <b>node</b> into *<b>ap_out</b>.  */
void
node_get_pref_dirport(const node_t *node, tor_addr_port_t *ap_out)
{
  tor_assert(ap_out);

  if (node_ipv6_dir_preferred(node)) {
    node_get_pref_ipv6_dirport(node, ap_out);
  } else {
    /* the primary DirPort is always on IPv4 */
    node_get_prim_dirport(node, ap_out);
  }
}

/** Copy the preferred IPv6 Dir port (IP address and TCP port) for
 * <b>node</b> into *<b>ap_out</b>. */
void
node_get_pref_ipv6_dirport(const node_t *node, tor_addr_port_t *ap_out)
{
  node_assert_ok(node);
  tor_assert(ap_out);

  /* Check ri first, because rewrite_node_address_for_bridge() updates
   * node->ri with the configured bridge address.
   * Prefer rs over md for consistency with the fascist_firewall_* functions.
   * Check if the address or port are valid, and try another alternative
   * if they are not. */

  /* Assume IPv4 and IPv6 dirports are the same */
  if (node->ri && tor_addr_port_is_valid(&node->ri->ipv6_addr,
                                         node->ri->dir_port, 0)) {
    tor_addr_copy(&ap_out->addr, &node->ri->ipv6_addr);
    ap_out->port = node->ri->dir_port;
  } else if (node->rs && tor_addr_port_is_valid(&node->rs->ipv6_addr,
                                                node->rs->dir_port, 0)) {
    tor_addr_copy(&ap_out->addr, &node->rs->ipv6_addr);
    ap_out->port = node->rs->dir_port;
  } else {
    tor_addr_make_null(&ap_out->addr, AF_INET6);
    ap_out->port = 0;
  }
}

/** Return true iff <b>md</b> has a curve25519 onion key.
 * Use node_has_curve25519_onion_key() instead of calling this directly. */
static int
microdesc_has_curve25519_onion_key(const microdesc_t *md)
{
  if (!md) {
    return 0;
  }

  if (!md->onion_curve25519_pkey) {
    return 0;
  }

  if (tor_mem_is_zero((const char*)md->onion_curve25519_pkey->public_key,
                      CURVE25519_PUBKEY_LEN)) {
    return 0;
  }

  return 1;
}

/** Return true iff <b>node</b> has a curve25519 onion key. */
int
node_has_curve25519_onion_key(const node_t *node)
{
  if (!node)
    return 0;

  if (node->ri)
    return routerinfo_has_curve25519_onion_key(node->ri);
  else if (node->md)
    return microdesc_has_curve25519_onion_key(node->md);
  else
    return 0;
}

/** Refresh the country code of <b>ri</b>.  This function MUST be called on
 * each router when the GeoIP database is reloaded, and on all new routers. */
void
node_set_country(node_t *node)
{
  tor_addr_t addr = TOR_ADDR_NULL;

  /* XXXXipv6 */
  if (node->rs)
    tor_addr_from_ipv4h(&addr, node->rs->addr);
  else if (node->ri)
    tor_addr_from_ipv4h(&addr, node->ri->addr);

  node->country = geoip_get_country_by_addr(&addr);
}

/** Set the country code of all routers in the routerlist. */
void
nodelist_refresh_countries(void)
{
  smartlist_t *nodes = nodelist_get_list();
  SMARTLIST_FOREACH(nodes, node_t *, node,
                    node_set_country(node));
}

/** Return true iff router1 and router2 have similar enough network addresses
 * that we should treat them as being in the same family */
int
addrs_in_same_network_family(const tor_addr_t *a1,
                             const tor_addr_t *a2)
{
  return 0 == tor_addr_compare_masked(a1, a2, 16, CMP_SEMANTIC);
}

/** Return true if <b>node</b>'s nickname matches <b>nickname</b>
 * (case-insensitive), or if <b>node's</b> identity key digest
 * matches a hexadecimal value stored in <b>nickname</b>.  Return
 * false otherwise. */
static int
node_nickname_matches(const node_t *node, const char *nickname)
{
  const char *n = node_get_nickname(node);
  if (n && nickname[0]!='$' && !strcasecmp(n, nickname))
    return 1;
  return hex_digest_nickname_matches(nickname,
                                     node->identity,
                                     n,
                                     node_is_named(node));
}

/** Return true iff <b>node</b> is named by some nickname in <b>lst</b>. */
static inline int
node_in_nickname_smartlist(const smartlist_t *lst, const node_t *node)
{
  if (!lst) return 0;
  SMARTLIST_FOREACH(lst, const char *, name, {
    if (node_nickname_matches(node, name))
      return 1;
  });
  return 0;
}

/** Return true iff r1 and r2 are in the same family, but not the same
 * router. */
int
nodes_in_same_family(const node_t *node1, const node_t *node2)
{
  const or_options_t *options = get_options();

  /* Are they in the same family because of their addresses? */
  if (options->EnforceDistinctSubnets) {
    tor_addr_t a1, a2;
    node_get_addr(node1, &a1);
    node_get_addr(node2, &a2);
    if (addrs_in_same_network_family(&a1, &a2))
      return 1;
  }

  /* Are they in the same family because the agree they are? */
  {
    const smartlist_t *f1, *f2;
    f1 = node_get_declared_family(node1);
    f2 = node_get_declared_family(node2);
    if (f1 && f2 &&
        node_in_nickname_smartlist(f1, node2) &&
        node_in_nickname_smartlist(f2, node1))
      return 1;
  }

  /* Are they in the same option because the user says they are? */
  if (options->NodeFamilySets) {
    SMARTLIST_FOREACH(options->NodeFamilySets, const routerset_t *, rs, {
        if (routerset_contains_node(rs, node1) &&
            routerset_contains_node(rs, node2))
          return 1;
      });
  }

  return 0;
}

/**
 * Add all the family of <b>node</b>, including <b>node</b> itself, to
 * the smartlist <b>sl</b>.
 *
 * This is used to make sure we don't pick siblings in a single path, or
 * pick more than one relay from a family for our entry guard list.
 * Note that a node may be added to <b>sl</b> more than once if it is
 * part of <b>node</b>'s family for more than one reason.
 */
void
nodelist_add_node_and_family(smartlist_t *sl, const node_t *node)
{
  const smartlist_t *all_nodes = nodelist_get_list();
  const smartlist_t *declared_family;
  const or_options_t *options = get_options();

  tor_assert(node);

  declared_family = node_get_declared_family(node);

  /* Let's make sure that we have the node itself, if it's a real node. */
  {
    const node_t *real_node = node_get_by_id(node->identity);
    if (real_node)
      smartlist_add(sl, (node_t*)real_node);
  }

  /* First, add any nodes with similar network addresses. */
  if (options->EnforceDistinctSubnets) {
    tor_addr_t node_addr;
    node_get_addr(node, &node_addr);

    SMARTLIST_FOREACH_BEGIN(all_nodes, const node_t *, node2) {
      tor_addr_t a;
      node_get_addr(node2, &a);
      if (addrs_in_same_network_family(&a, &node_addr))
        smartlist_add(sl, (void*)node2);
    } SMARTLIST_FOREACH_END(node2);
  }

  /* Now, add all nodes in the declared_family of this node, if they
   * also declare this node to be in their family. */
  if (declared_family) {
    /* Add every r such that router declares familyness with node, and node
     * declares familyhood with router. */
    SMARTLIST_FOREACH_BEGIN(declared_family, const char *, name) {
      const node_t *node2;
      const smartlist_t *family2;
      if (!(node2 = node_get_by_nickname(name, 0)))
        continue;
      if (!(family2 = node_get_declared_family(node2)))
        continue;
      SMARTLIST_FOREACH_BEGIN(family2, const char *, name2) {
          if (node_nickname_matches(node, name2)) {
            smartlist_add(sl, (void*)node2);
            break;
          }
      } SMARTLIST_FOREACH_END(name2);
    } SMARTLIST_FOREACH_END(name);
  }

  /* If the user declared any families locally, honor those too. */
  if (options->NodeFamilySets) {
    SMARTLIST_FOREACH(options->NodeFamilySets, const routerset_t *, rs, {
      if (routerset_contains_node(rs, node)) {
        routerset_get_all_nodes(sl, rs, NULL, 0);
      }
    });
  }
}

/** Find a router that's up, that has this IP address, and
 * that allows exit to this address:port, or return NULL if there
 * isn't a good one.
 * Don't exit enclave to excluded relays -- it wouldn't actually
 * hurt anything, but this way there are fewer confused users.
 */
const node_t *
router_find_exact_exit_enclave(const char *address, uint16_t port)
{/*XXXX MOVE*/
  uint32_t addr;
  struct in_addr in;
  tor_addr_t a;
  const or_options_t *options = get_options();

  if (!tor_inet_aton(address, &in))
    return NULL; /* it's not an IP already */
  addr = ntohl(in.s_addr);

  tor_addr_from_ipv4h(&a, addr);

  SMARTLIST_FOREACH(nodelist_get_list(), const node_t *, node, {
    if (node_get_addr_ipv4h(node) == addr &&
        node->is_running &&
        compare_tor_addr_to_node_policy(&a, port, node) ==
          ADDR_POLICY_ACCEPTED &&
        !routerset_contains_node(options->ExcludeExitNodesUnion_, node))
      return node;
  });
  return NULL;
}

/** Return 1 if <b>router</b> is not suitable for these parameters, else 0.
 * If <b>need_uptime</b> is non-zero, we require a minimum uptime.
 * If <b>need_capacity</b> is non-zero, we require a minimum advertised
 * bandwidth.
 * If <b>need_guard</b>, we require that the router is a possible entry guard.
 */
int
node_is_unreliable(const node_t *node, int need_uptime,
                   int need_capacity, int need_guard)
{
  if (need_uptime && !node->is_stable)
    return 1;
  if (need_capacity && !node->is_fast)
    return 1;
  if (need_guard && !node->is_possible_guard)
    return 1;
  return 0;
}

/** Return 1 if all running sufficiently-stable routers we can use will reject
 * addr:port. Return 0 if any might accept it. */
int
router_exit_policy_all_nodes_reject(const tor_addr_t *addr, uint16_t port,
                                    int need_uptime)
{
  addr_policy_result_t r;

  SMARTLIST_FOREACH_BEGIN(nodelist_get_list(), const node_t *, node) {
    if (node->is_running &&
        !node_is_unreliable(node, need_uptime, 0, 0)) {

      r = compare_tor_addr_to_node_policy(addr, port, node);

      if (r != ADDR_POLICY_REJECTED && r != ADDR_POLICY_PROBABLY_REJECTED)
        return 0; /* this one could be ok. good enough. */
    }
  } SMARTLIST_FOREACH_END(node);
  return 1; /* all will reject. */
}

/** Mark the router with ID <b>digest</b> as running or non-running
 * in our routerlist. */
void
router_set_status(const char *digest, int up)
{
  node_t *node;
  tor_assert(digest);

  SMARTLIST_FOREACH(router_get_fallback_dir_servers(),
                    dir_server_t *, d,
                    if (tor_memeq(d->digest, digest, DIGEST_LEN))
                      d->is_running = up);

  SMARTLIST_FOREACH(router_get_trusted_dir_servers(),
                    dir_server_t *, d,
                    if (tor_memeq(d->digest, digest, DIGEST_LEN))
                      d->is_running = up);

  node = node_get_mutable_by_id(digest);
  if (node) {
#if 0
    log_debug(LD_DIR,"Marking router %s as %s.",
              node_describe(node), up ? "up" : "down");
#endif
    if (!up && node_is_me(node) && !net_is_disabled())
      log_warn(LD_NET, "We just marked ourself as down. Are your external "
               "addresses reachable?");

    if (bool_neq(node->is_running, up))
      router_dir_info_changed();

    node->is_running = up;
  }
}

/** True iff, the last time we checked whether we had enough directory info
 * to build circuits, the answer was "yes". If there are no exits in the
 * consensus, we act as if we have 100% of the exit directory info. */
static int have_min_dir_info = 0;

/** Does the consensus contain nodes that can exit? */
static consensus_path_type_t have_consensus_path = CONSENSUS_PATH_UNKNOWN;

/** True iff enough has changed since the last time we checked whether we had
 * enough directory info to build circuits that our old answer can no longer
 * be trusted. */
static int need_to_update_have_min_dir_info = 1;
/** String describing what we're missing before we have enough directory
 * info. */
static char dir_info_status[512] = "";

/** Return true iff we have enough consensus information to
 * start building circuits.  Right now, this means "a consensus that's
 * less than a day old, and at least 60% of router descriptors (configurable),
 * weighted by bandwidth. Treat the exit fraction as 100% if there are
 * no exits in the consensus."
 * To obtain the final weighted bandwidth, we multiply the
 * weighted bandwidth fraction for each position (guard, middle, exit). */
int
router_have_minimum_dir_info(void)
{
  static int logged_delay=0;
  const char *delay_fetches_msg = NULL;
  if (should_delay_dir_fetches(get_options(), &delay_fetches_msg)) {
    if (!logged_delay)
      log_notice(LD_DIR, "Delaying directory fetches: %s", delay_fetches_msg);
    logged_delay=1;
    strlcpy(dir_info_status, delay_fetches_msg,  sizeof(dir_info_status));
    return 0;
  }
  logged_delay = 0; /* reset it if we get this far */

  if (PREDICT_UNLIKELY(need_to_update_have_min_dir_info)) {
    update_router_have_minimum_dir_info();
  }

  return have_min_dir_info;
}

/** Set to CONSENSUS_PATH_EXIT if there is at least one exit node
 * in the consensus. We update this flag in compute_frac_paths_available if
 * there is at least one relay that has an Exit flag in the consensus.
 * Used to avoid building exit circuits when they will almost certainly fail.
 * Set to CONSENSUS_PATH_INTERNAL if there are no exits in the consensus.
 * (This situation typically occurs during bootstrap of a test network.)
 * Set to CONSENSUS_PATH_UNKNOWN if we have never checked, or have
 * reason to believe our last known value was invalid or has expired.
 * If we're in a network with TestingDirAuthVoteExit set,
 * this can cause router_have_consensus_path() to be set to
 * CONSENSUS_PATH_EXIT, even if there are no nodes with accept exit policies.
 */
MOCK_IMPL(consensus_path_type_t,
router_have_consensus_path, (void))
{
  return have_consensus_path;
}

/** Called when our internal view of the directory has changed.  This can be
 * when the authorities change, networkstatuses change, the list of routerdescs
 * changes, or number of running routers changes.
 */
void
router_dir_info_changed(void)
{
  need_to_update_have_min_dir_info = 1;
  rend_hsdir_routers_changed();
}

/** Return a string describing what we're missing before we have enough
 * directory info. */
const char *
get_dir_info_status_string(void)
{
  return dir_info_status;
}

/** Iterate over the servers listed in <b>consensus</b>, and count how many of
 * them seem like ones we'd use (store this in *<b>num_usable</b>), and how
 * many of <em>those</em> we have descriptors for (store this in
 * *<b>num_present</b>).
 *
 * If <b>in_set</b> is non-NULL, only consider those routers in <b>in_set</b>.
 * If <b>exit_only</b> is USABLE_DESCRIPTOR_EXIT_ONLY, only consider nodes
 * with the Exit flag.
 * If *<b>descs_out</b> is present, add a node_t for each usable descriptor
 * to it.
 */
static void
count_usable_descriptors(int *num_present, int *num_usable,
                         smartlist_t *descs_out,
                         const networkstatus_t *consensus,
                         time_t now,
                         routerset_t *in_set,
                         usable_descriptor_t exit_only)
{
  const int md = (consensus->flavor == FLAV_MICRODESC);
  *num_present = 0, *num_usable = 0;

  SMARTLIST_FOREACH_BEGIN(consensus->routerstatus_list, routerstatus_t *, rs)
    {
       const node_t *node = node_get_by_id(rs->identity_digest);
       if (!node)
         continue; /* This would be a bug: every entry in the consensus is
                    * supposed to have a node. */
       if (exit_only == USABLE_DESCRIPTOR_EXIT_ONLY && ! rs->is_exit)
         continue;
       if (in_set && ! routerset_contains_routerstatus(in_set, rs, -1))
         continue;
       if (client_would_use_router(rs, now)) {
         const char * const digest = rs->descriptor_digest;
         int present;
         ++*num_usable; /* the consensus says we want it. */
         if (md)
           present = NULL != microdesc_cache_lookup_by_digest256(NULL, digest);
         else
           present = NULL != router_get_by_descriptor_digest(digest);
         if (present) {
           /* we have the descriptor listed in the consensus. */
           ++*num_present;
         }
         if (descs_out)
           smartlist_add(descs_out, (node_t*)node);
       }
     }
  SMARTLIST_FOREACH_END(rs);

  log_debug(LD_DIR, "%d usable, %d present (%s%s).",
            *num_usable, *num_present,
            md ? "microdesc" : "desc",
            exit_only == USABLE_DESCRIPTOR_EXIT_ONLY ? " exits" : "s");
}

/** Return an estimate of which fraction of usable paths through the Tor
 * network we have available for use.  Count how many routers seem like ones
 * we'd use (store this in *<b>num_usable_out</b>), and how many of
 * <em>those</em> we have descriptors for (store this in
 * *<b>num_present_out</b>.)
 *
 * If **<b>status_out</b> is present, allocate a new string and print the
 * available percentages of guard, middle, and exit nodes to it, noting
 * whether there are exits in the consensus.
 * If there are no exits in the consensus, we treat the exit fraction as 100%,
 * but set router_have_consensus_path() so that we can only build internal
 * paths. */
static double
compute_frac_paths_available(const networkstatus_t *consensus,
                             const or_options_t *options, time_t now,
                             int *num_present_out, int *num_usable_out,
                             char **status_out)
{
  smartlist_t *guards = smartlist_new();
  smartlist_t *mid    = smartlist_new();
  smartlist_t *exits  = smartlist_new();
  double f_guard, f_mid, f_exit;
  double f_path = 0.0;
  /* Used to determine whether there are any exits in the consensus */
  int np = 0;
  /* Used to determine whether there are any exits with descriptors */
  int nu = 0;
  const int authdir = authdir_mode_v3(options);

  count_usable_descriptors(num_present_out, num_usable_out,
                           mid, consensus, now, NULL,
                           USABLE_DESCRIPTOR_ALL);
  if (options->EntryNodes) {
    count_usable_descriptors(&np, &nu, guards, consensus, now,
                             options->EntryNodes, USABLE_DESCRIPTOR_ALL);
  } else {
    SMARTLIST_FOREACH(mid, const node_t *, node, {
      if (authdir) {
        if (node->rs && node->rs->is_possible_guard)
          smartlist_add(guards, (node_t*)node);
      } else {
        if (node->is_possible_guard)
          smartlist_add(guards, (node_t*)node);
      }
    });
  }

  /* All nodes with exit flag
   * If we're in a network with TestingDirAuthVoteExit set,
   * this can cause false positives on have_consensus_path,
   * incorrectly setting it to CONSENSUS_PATH_EXIT. This is
   * an unavoidable feature of forcing authorities to declare
   * certain nodes as exits.
   */
  count_usable_descriptors(&np, &nu, exits, consensus, now,
                           NULL, USABLE_DESCRIPTOR_EXIT_ONLY);
  log_debug(LD_NET,
            "%s: %d present, %d usable",
            "exits",
            np,
            nu);

  /* We need at least 1 exit present in the consensus to consider
   * building exit paths */
  /* Update our understanding of whether the consensus has exits */
  consensus_path_type_t old_have_consensus_path = have_consensus_path;
  have_consensus_path = ((nu > 0) ?
                         CONSENSUS_PATH_EXIT :
                         CONSENSUS_PATH_INTERNAL);

  if (have_consensus_path == CONSENSUS_PATH_INTERNAL
      && old_have_consensus_path != have_consensus_path) {
    log_notice(LD_NET,
               "The current consensus has no exit nodes. "
               "Tor can only build internal paths, "
               "such as paths to hidden services.");

    /* However, exit nodes can reachability self-test using this consensus,
     * join the network, and appear in a later consensus. This will allow
     * the network to build exit paths, such as paths for world wide web
     * browsing (as distinct from hidden service web browsing). */
  }

  f_guard = frac_nodes_with_descriptors(guards, WEIGHT_FOR_GUARD);
  f_mid   = frac_nodes_with_descriptors(mid,    WEIGHT_FOR_MID);
  f_exit  = frac_nodes_with_descriptors(exits,  WEIGHT_FOR_EXIT);

  log_debug(LD_NET,
            "f_guard: %.2f, f_mid: %.2f, f_exit: %.2f",
             f_guard,
             f_mid,
             f_exit);

  smartlist_free(guards);
  smartlist_free(mid);
  smartlist_free(exits);

  if (options->ExitNodes) {
    double f_myexit, f_myexit_unflagged;
    smartlist_t *myexits= smartlist_new();
    smartlist_t *myexits_unflagged = smartlist_new();

    /* All nodes with exit flag in ExitNodes option */
    count_usable_descriptors(&np, &nu, myexits, consensus, now,
                             options->ExitNodes, USABLE_DESCRIPTOR_EXIT_ONLY);
    log_debug(LD_NET,
              "%s: %d present, %d usable",
              "myexits",
              np,
              nu);

    /* Now compute the nodes in the ExitNodes option where which we don't know
     * what their exit policy is, or we know it permits something. */
    count_usable_descriptors(&np, &nu, myexits_unflagged,
                             consensus, now,
                             options->ExitNodes, USABLE_DESCRIPTOR_ALL);
    log_debug(LD_NET,
              "%s: %d present, %d usable",
              "myexits_unflagged (initial)",
              np,
              nu);

    SMARTLIST_FOREACH_BEGIN(myexits_unflagged, const node_t *, node) {
      if (node_has_descriptor(node) && node_exit_policy_rejects_all(node)) {
        SMARTLIST_DEL_CURRENT(myexits_unflagged, node);
        /* this node is not actually an exit */
        np--;
        /* this node is unusable as an exit */
        nu--;
      }
    } SMARTLIST_FOREACH_END(node);

    log_debug(LD_NET,
              "%s: %d present, %d usable",
              "myexits_unflagged (final)",
              np,
              nu);

    f_myexit= frac_nodes_with_descriptors(myexits,WEIGHT_FOR_EXIT);
    f_myexit_unflagged=
              frac_nodes_with_descriptors(myexits_unflagged,WEIGHT_FOR_EXIT);

    log_debug(LD_NET,
              "f_exit: %.2f, f_myexit: %.2f, f_myexit_unflagged: %.2f",
              f_exit,
              f_myexit,
              f_myexit_unflagged);

    /* If our ExitNodes list has eliminated every possible Exit node, and there
     * were some possible Exit nodes, then instead consider nodes that permit
     * exiting to some ports. */
    if (smartlist_len(myexits) == 0 &&
        smartlist_len(myexits_unflagged)) {
      f_myexit = f_myexit_unflagged;
    }

    smartlist_free(myexits);
    smartlist_free(myexits_unflagged);

    /* This is a tricky point here: we don't want to make it easy for a
     * directory to trickle exits to us until it learns which exits we have
     * configured, so require that we have a threshold both of total exits
     * and usable exits. */
    if (f_myexit < f_exit)
      f_exit = f_myexit;
  }

  /* if the consensus has no exits, treat the exit fraction as 100% */
  if (router_have_consensus_path() != CONSENSUS_PATH_EXIT) {
    f_exit = 1.0;
  }

  f_path = f_guard * f_mid * f_exit;

  if (status_out)
    tor_asprintf(status_out,
                 "%d%% of guards bw, "
                 "%d%% of midpoint bw, and "
                 "%d%% of exit bw%s = "
                 "%d%% of path bw",
                 (int)(f_guard*100),
                 (int)(f_mid*100),
                 (int)(f_exit*100),
                 (router_have_consensus_path() == CONSENSUS_PATH_EXIT ?
                  "" :
                  " (no exits in consensus)"),
                 (int)(f_path*100));

  return f_path;
}

/** We just fetched a new set of descriptors. Compute how far through
 * the "loading descriptors" bootstrapping phase we are, so we can inform
 * the controller of our progress. */
int
count_loading_descriptors_progress(void)
{
  int num_present = 0, num_usable=0;
  time_t now = time(NULL);
  const or_options_t *options = get_options();
  const networkstatus_t *consensus =
    networkstatus_get_reasonably_live_consensus(now,usable_consensus_flavor());
  double paths, fraction;

  if (!consensus)
    return 0; /* can't count descriptors if we have no list of them */

  paths = compute_frac_paths_available(consensus, options, now,
                                       &num_present, &num_usable,
                                       NULL);

  fraction = paths / get_frac_paths_needed_for_circs(options,consensus);
  if (fraction > 1.0)
    return 0; /* it's not the number of descriptors holding us back */
  return BOOTSTRAP_STATUS_LOADING_DESCRIPTORS + (int)
    (fraction*(BOOTSTRAP_STATUS_CONN_OR-1 -
               BOOTSTRAP_STATUS_LOADING_DESCRIPTORS));
}

/** Return the fraction of paths needed before we're willing to build
 * circuits, as configured in <b>options</b>, or in the consensus <b>ns</b>. */
static double
get_frac_paths_needed_for_circs(const or_options_t *options,
                                const networkstatus_t *ns)
{
#define DFLT_PCT_USABLE_NEEDED 60
  if (options->PathsNeededToBuildCircuits >= 0.0) {
    return options->PathsNeededToBuildCircuits;
  } else {
    return networkstatus_get_param(ns, "min_paths_for_circs_pct",
                                   DFLT_PCT_USABLE_NEEDED,
                                   25, 95)/100.0;
  }
}

/** Change the value of have_min_dir_info, setting it true iff we have enough
 * network and router information to build circuits.  Clear the value of
 * need_to_update_have_min_dir_info. */
static void
update_router_have_minimum_dir_info(void)
{
  time_t now = time(NULL);
  int res;
  const or_options_t *options = get_options();
  const networkstatus_t *consensus =
    networkstatus_get_reasonably_live_consensus(now,usable_consensus_flavor());
  int using_md;

  if (!consensus) {
    if (!networkstatus_get_latest_consensus())
      strlcpy(dir_info_status, "We have no usable consensus.",
              sizeof(dir_info_status));
    else
      strlcpy(dir_info_status, "We have no recent usable consensus.",
              sizeof(dir_info_status));
    res = 0;
    goto done;
  }

  using_md = consensus->flavor == FLAV_MICRODESC;

  if (! entry_guards_have_enough_dir_info_to_build_circuits()) {
    strlcpy(dir_info_status, "We're missing descriptors for some of our "
            "primary entry guards", sizeof(dir_info_status));
    res = 0;
    goto done;
  }

  /* Check fraction of available paths */
  {
    char *status = NULL;
    int num_present=0, num_usable=0;
    double paths = compute_frac_paths_available(consensus, options, now,
                                                &num_present, &num_usable,
                                                &status);

    if (paths < get_frac_paths_needed_for_circs(options,consensus)) {
      tor_snprintf(dir_info_status, sizeof(dir_info_status),
                   "We need more %sdescriptors: we have %d/%d, and "
                   "can only build %d%% of likely paths. (We have %s.)",
                   using_md?"micro":"", num_present, num_usable,
                   (int)(paths*100), status);
      tor_free(status);
      res = 0;
      control_event_bootstrap(BOOTSTRAP_STATUS_REQUESTING_DESCRIPTORS, 0);
      goto done;
    }

    tor_free(status);
    res = 1;
  }

 done:

  /* If paths have just become available in this update. */
  if (res && !have_min_dir_info) {
    control_event_client_status(LOG_NOTICE, "ENOUGH_DIR_INFO");
    if (control_event_bootstrap(BOOTSTRAP_STATUS_CONN_OR, 0) == 0) {
      log_notice(LD_DIR,
              "We now have enough directory information to build circuits.");
    }
  }

  /* If paths have just become unavailable in this update. */
  if (!res && have_min_dir_info) {
    int quiet = directory_too_idle_to_fetch_descriptors(options, now);
    tor_log(quiet ? LOG_INFO : LOG_NOTICE, LD_DIR,
        "Our directory information is no longer up-to-date "
        "enough to build circuits: %s", dir_info_status);

    /* a) make us log when we next complete a circuit, so we know when Tor
     * is back up and usable, and b) disable some activities that Tor
     * should only do while circuits are working, like reachability tests
     * and fetching bridge descriptors only over circuits. */
    note_that_we_maybe_cant_complete_circuits();
    have_consensus_path = CONSENSUS_PATH_UNKNOWN;
    control_event_client_status(LOG_NOTICE, "NOT_ENOUGH_DIR_INFO");
  }
  have_min_dir_info = res;
  need_to_update_have_min_dir_info = 0;
}

