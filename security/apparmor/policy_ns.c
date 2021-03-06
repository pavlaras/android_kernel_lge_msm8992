/*
 * AppArmor security module
 *
 * This file contains AppArmor policy manipulation functions
 *
 * Copyright (C) 1998-2008 Novell/SUSE
 * Copyright 2009-2017 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * AppArmor policy namespaces, allow for different sets of policies
 * to be loaded for tasks within the namespace.
 */

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "include/apparmor.h"
#include "include/context.h"
#include "include/policy_ns.h"
#include "include/policy.h"

/* root profile namespace */
struct aa_namespace *root_ns;
const char *aa_hidden_ns_name = "---";

/**
 * aa_ns_visible - test if @view is visible from @curr
 * @curr: namespace to treat as the parent (NOT NULL)
 * @view: namespace to test if visible from @curr (NOT NULL)
 *
 * Returns: true if @view is visible from @curr else false
 */
bool aa_ns_visible(struct aa_namespace *curr, struct aa_namespace *view)
{
	if (curr == view)
		return true;

	for ( ; view; view = view->parent) {
		if (view->parent == curr)
			return true;
	}
	return false;
}

/**
 * aa_na_name - Find the ns name to display for @view from @curr
 * @curr - current namespace (NOT NULL)
 * @view - namespace attempting to view (NOT NULL)
 *
 * Returns: name of @view visible from @curr
 */
const char *aa_ns_name(struct aa_namespace *curr, struct aa_namespace *view)
{
	/* if view == curr then the namespace name isn't displayed */
	if (curr == view)
		return "";

	if (aa_ns_visible(curr, view)) {
		/* at this point if a ns is visible it is in a view ns
		 * thus the curr ns.hname is a prefix of its name.
		 * Only output the virtualized portion of the name
		 * Add + 2 to skip over // separating curr hname prefix
		 * from the visible tail of the views hname
		 */
		return view->base.hname + strlen(curr->base.hname) + 2;
	}

	return aa_hidden_ns_name;
}

/**
 * alloc_namespace - allocate, initialize and return a new namespace
 * @prefix: parent namespace name (MAYBE NULL)
 * @name: a preallocated name  (NOT NULL)
 *
 * Returns: refcounted namespace or NULL on failure.
 */
static struct aa_namespace *alloc_namespace(const char *prefix,
					    const char *name)
{
	struct aa_namespace *ns;

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	AA_DEBUG("%s(%p)\n", __func__, ns);
	if (!ns)
		return NULL;
	if (!aa_policy_init(&ns->base, prefix, name))
		goto fail_ns;

	INIT_LIST_HEAD(&ns->sub_ns);
	mutex_init(&ns->lock);

	/* released by free_namespace */
	ns->unconfined = aa_alloc_profile("unconfined");
	if (!ns->unconfined)
		goto fail_unconfined;

	ns->unconfined->flags = PFLAG_IX_ON_NAME_ERROR |
		PFLAG_IMMUTABLE | PFLAG_NS_COUNT;
	ns->unconfined->mode = APPARMOR_UNCONFINED;

	/* ns and ns->unconfined share ns->unconfined refcount */
	ns->unconfined->ns = ns;

	atomic_set(&ns->uniq_null, 0);

	return ns;

fail_unconfined:
	kzfree(ns->base.hname);
fail_ns:
	kzfree(ns);
	return NULL;
}

/**
 * aa_free_namespace - free a profile namespace
 * @ns: the namespace to free  (MAYBE NULL)
 *
 * Requires: All references to the namespace must have been put, if the
 *           namespace was referenced by a profile confining a task,
 */
void aa_free_namespace(struct aa_namespace *ns)
{
	if (!ns)
		return;

	aa_policy_destroy(&ns->base);
	aa_put_namespace(ns->parent);

	ns->unconfined->ns = NULL;
	aa_free_profile(ns->unconfined);
	kzfree(ns);
}

/**
 * aa_find_namespace  -  look up a profile namespace on the namespace list
 * @root: namespace to search in  (NOT NULL)
 * @name: name of namespace to find  (NOT NULL)
 *
 * Returns: a refcounted namespace on the list, or NULL if no namespace
 *          called @name exists.
 *
 * refcount released by caller
 */
struct aa_namespace *aa_find_namespace(struct aa_namespace *root,
				       const char *name)
{
	struct aa_namespace *ns = NULL;

	rcu_read_lock();
	ns = aa_get_namespace(__aa_find_namespace(&root->sub_ns, name));
	rcu_read_unlock();

	return ns;
}

/**
 * aa_prepare_namespace - find an existing or create a new namespace of @name
 * @name: the namespace to find or add  (MAYBE NULL)
 *
 * Returns: refcounted namespace or NULL if failed to create one
 */
struct aa_namespace *aa_prepare_namespace(const char *name)
{
	struct aa_namespace *ns, *root;

	root = aa_current_profile()->ns;

	mutex_lock(&root->lock);

	/* if name isn't specified the profile is loaded to the current ns */
	if (!name) {
		/* released by caller */
		ns = aa_get_namespace(root);
		goto out;
	}

	/* try and find the specified ns and if it doesn't exist create it */
	/* released by caller */
	ns = aa_get_namespace(__aa_find_namespace(&root->sub_ns, name));
	if (!ns) {
		ns = alloc_namespace(root->base.hname, name);
		if (!ns)
			goto out;
		if (__aa_fs_namespace_mkdir(ns, ns_subns_dir(root), name)) {
			AA_ERROR("Failed to create interface for ns %s\n",
				 ns->base.name);
			aa_free_namespace(ns);
			ns = NULL;
			goto out;
		}
		ns->parent = aa_get_namespace(root);
		list_add_rcu(&ns->base.list, &root->sub_ns);
		/* add list ref */
		aa_get_namespace(ns);
	}
out:
	mutex_unlock(&root->lock);

	/* return ref */
	return ns;
}

static void __ns_list_release(struct list_head *head);

/**
 * destroy_namespace - remove everything contained by @ns
 * @ns: namespace to have it contents removed  (NOT NULL)
 */
static void destroy_namespace(struct aa_namespace *ns)
{
	if (!ns)
		return;

	mutex_lock(&ns->lock);
	/* release all profiles in this namespace */
	__aa_profile_list_release(&ns->base.profiles);

	/* release all sub namespaces */
	__ns_list_release(&ns->sub_ns);

	if (ns->parent)
		__aa_update_replacedby(ns->unconfined, ns->parent->unconfined);
	__aa_fs_namespace_rmdir(ns);
	mutex_unlock(&ns->lock);
}

/**
 * __aa_remove_namespace - remove a namespace and all its children
 * @ns: namespace to be removed  (NOT NULL)
 *
 * Requires: ns->parent->lock be held and ns removed from parent.
 */
void __aa_remove_namespace(struct aa_namespace *ns)
{
	/* remove ns from namespace list */
	list_del_rcu(&ns->base.list);
	destroy_namespace(ns);
	aa_put_namespace(ns);
}

/**
 * __ns_list_release - remove all profile namespaces on the list put refs
 * @head: list of profile namespaces  (NOT NULL)
 *
 * Requires: namespace lock be held
 */
static void __ns_list_release(struct list_head *head)
{
	struct aa_namespace *ns, *tmp;

	list_for_each_entry_safe(ns, tmp, head, base.list)
		__aa_remove_namespace(ns);

}

/**
 * aa_alloc_root_ns - allocate the root profile namespace
 *
 * Returns: %0 on success else error
 *
 */
int __init aa_alloc_root_ns(void)
{
	/* released by aa_free_root_ns - used as list ref*/
	root_ns = alloc_namespace(NULL, "root");
	if (!root_ns)
		return -ENOMEM;

	return 0;
}

 /**
  * aa_free_root_ns - free the root profile namespace
  */
void __init aa_free_root_ns(void)
{
	 struct aa_namespace *ns = root_ns;

	 root_ns = NULL;

	 destroy_namespace(ns);
	 aa_put_namespace(ns);
}
