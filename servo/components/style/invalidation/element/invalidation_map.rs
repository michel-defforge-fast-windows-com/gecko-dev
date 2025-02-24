/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! Code for invalidations due to state or attribute changes.

use crate::context::QuirksMode;
use crate::element_state::{DocumentState, ElementState};
use crate::selector_map::{MaybeCaseInsensitiveHashMap, SelectorMap, SelectorMapEntry};
use crate::selector_parser::SelectorImpl;
use crate::{Atom, LocalName, Namespace};
use fallible::FallibleVec;
use hashglobe::FailedAllocationError;
use selectors::attr::NamespaceConstraint;
use selectors::parser::{Combinator, Component};
use selectors::parser::{Selector, SelectorIter, Visit};
use selectors::visitor::SelectorVisitor;
use smallvec::SmallVec;

/// Mapping between (partial) CompoundSelectors (and the combinator to their
/// right) and the states and attributes they depend on.
///
/// In general, for all selectors in all applicable stylesheets of the form:
///
/// |a _ b _ c _ d _ e|
///
/// Where:
///   * |b| and |d| are simple selectors that depend on state (like :hover) or
///     attributes (like [attr...], .foo, or #foo).
///   * |a|, |c|, and |e| are arbitrary simple selectors that do not depend on
///     state or attributes.
///
/// We generate a Dependency for both |a _ b:X _| and |a _ b:X _ c _ d:Y _|,
/// even though those selectors may not appear on their own in any stylesheet.
/// This allows us to quickly scan through the dependency sites of all style
/// rules and determine the maximum effect that a given state or attribute
/// change may have on the style of elements in the document.
#[derive(Clone, Debug, MallocSizeOf)]
pub struct Dependency {
    /// The dependency selector.
    #[cfg_attr(
        feature = "gecko",
        ignore_malloc_size_of = "CssRules have primary refs, we measure there"
    )]
    #[cfg_attr(feature = "servo", ignore_malloc_size_of = "Arc")]
    pub selector: Selector<SelectorImpl>,

    /// The offset into the selector that we should match on.
    pub selector_offset: usize,
}

/// The kind of elements down the tree this dependency may affect.
#[derive(Debug, Eq, PartialEq)]
pub enum DependencyInvalidationKind {
    /// This dependency may affect the element that changed itself.
    Element,
    /// This dependency affects the style of the element itself, and also the
    /// style of its descendants.
    ///
    /// TODO(emilio): Each time this feels more of a hack for eager pseudos...
    ElementAndDescendants,
    /// This dependency may affect descendants down the tree.
    Descendants,
    /// This dependency may affect siblings to the right of the element that
    /// changed.
    Siblings,
    /// This dependency may affect slotted elements of the element that changed.
    SlottedElements,
    /// This dependency may affect parts of the element that changed.
    Parts,
}

impl Dependency {
    /// Returns the combinator to the right of the partial selector this
    /// dependency represents.
    ///
    /// TODO(emilio): Consider storing inline if it helps cache locality?
    pub fn combinator(&self) -> Option<Combinator> {
        if self.selector_offset == 0 {
            return None;
        }

        Some(
            self.selector
                .combinator_at_match_order(self.selector_offset - 1),
        )
    }

    /// The kind of invalidation that this would generate.
    pub fn invalidation_kind(&self) -> DependencyInvalidationKind {
        match self.combinator() {
            None => DependencyInvalidationKind::Element,
            Some(Combinator::Child) | Some(Combinator::Descendant) => {
                DependencyInvalidationKind::Descendants
            },
            Some(Combinator::LaterSibling) | Some(Combinator::NextSibling) => {
                DependencyInvalidationKind::Siblings
            },
            // TODO(emilio): We could look at the selector itself to see if it's
            // an eager pseudo, and return only Descendants here if not.
            Some(Combinator::PseudoElement) => DependencyInvalidationKind::ElementAndDescendants,
            Some(Combinator::SlotAssignment) => DependencyInvalidationKind::SlottedElements,
            Some(Combinator::Part) => DependencyInvalidationKind::Parts,
        }
    }
}

impl SelectorMapEntry for Dependency {
    fn selector(&self) -> SelectorIter<SelectorImpl> {
        self.selector.iter_from(self.selector_offset)
    }
}

/// The same, but for state selectors, which can track more exactly what state
/// do they track.
#[derive(Clone, Debug, MallocSizeOf)]
pub struct StateDependency {
    /// The other dependency fields.
    pub dep: Dependency,
    /// The state this dependency is affected by.
    pub state: ElementState,
}

impl SelectorMapEntry for StateDependency {
    fn selector(&self) -> SelectorIter<SelectorImpl> {
        self.dep.selector()
    }
}

/// The same, but for document state selectors.
#[derive(Clone, Debug, MallocSizeOf)]
pub struct DocumentStateDependency {
    /// The selector that is affected. We don't need to track an offset, since
    /// when it changes it changes for the whole document anyway.
    #[cfg_attr(
        feature = "gecko",
        ignore_malloc_size_of = "CssRules have primary refs, we measure there"
    )]
    #[cfg_attr(feature = "servo", ignore_malloc_size_of = "Arc")]
    pub selector: Selector<SelectorImpl>,
    /// The state this dependency is affected by.
    pub state: DocumentState,
}

bitflags! {
    /// A set of flags that denote whether any invalidations have occurred
    /// for a particular attribute selector.
    #[derive(MallocSizeOf)]
    #[repr(C)]
    pub struct InvalidationMapFlags : u8 {
        /// Whether [class] or such is used.
        const HAS_CLASS_ATTR_SELECTOR = 1 << 0;
        /// Whether [id] or such is used.
        const HAS_ID_ATTR_SELECTOR = 1 << 1;
    }
}

/// A map where we store invalidations.
///
/// This is slightly different to a SelectorMap, in the sense of that the same
/// selector may appear multiple times.
///
/// In particular, we want to lookup as few things as possible to get the fewer
/// selectors the better, so this looks up by id, class, or looks at the list of
/// state/other attribute affecting selectors.
#[derive(Debug, MallocSizeOf)]
pub struct InvalidationMap {
    /// A map from a given class name to all the selectors with that class
    /// selector.
    pub class_to_selector: MaybeCaseInsensitiveHashMap<Atom, SmallVec<[Dependency; 1]>>,
    /// A map from a given id to all the selectors with that ID in the
    /// stylesheets currently applying to the document.
    pub id_to_selector: MaybeCaseInsensitiveHashMap<Atom, SmallVec<[Dependency; 1]>>,
    /// A map of all the state dependencies.
    pub state_affecting_selectors: SelectorMap<StateDependency>,
    /// A list of document state dependencies in the rules we represent.
    pub document_state_selectors: Vec<DocumentStateDependency>,
    /// A map of other attribute affecting selectors.
    pub other_attribute_affecting_selectors: SelectorMap<Dependency>,
    /// A set of flags that contain whether various special attributes are used
    /// in this invalidation map.
    pub flags: InvalidationMapFlags,
}

impl InvalidationMap {
    /// Creates an empty `InvalidationMap`.
    pub fn new() -> Self {
        Self {
            class_to_selector: MaybeCaseInsensitiveHashMap::new(),
            id_to_selector: MaybeCaseInsensitiveHashMap::new(),
            state_affecting_selectors: SelectorMap::new(),
            document_state_selectors: Vec::new(),
            other_attribute_affecting_selectors: SelectorMap::new(),
            flags: InvalidationMapFlags::empty(),
        }
    }

    /// Returns the number of dependencies stored in the invalidation map.
    pub fn len(&self) -> usize {
        self.state_affecting_selectors.len() +
            self.document_state_selectors.len() +
            self.other_attribute_affecting_selectors.len() +
            self.id_to_selector
                .iter()
                .fold(0, |accum, (_, ref v)| accum + v.len()) +
            self.class_to_selector
                .iter()
                .fold(0, |accum, (_, ref v)| accum + v.len())
    }

    /// Clears this map, leaving it empty.
    pub fn clear(&mut self) {
        self.class_to_selector.clear();
        self.id_to_selector.clear();
        self.state_affecting_selectors.clear();
        self.document_state_selectors.clear();
        self.other_attribute_affecting_selectors.clear();
        self.flags = InvalidationMapFlags::empty();
    }

    /// Adds a selector to this `InvalidationMap`.  Returns Err(..) to
    /// signify OOM.
    pub fn note_selector(
        &mut self,
        selector: &Selector<SelectorImpl>,
        quirks_mode: QuirksMode,
    ) -> Result<(), FailedAllocationError> {
        debug!("InvalidationMap::note_selector({:?})", selector);

        let mut iter = selector.iter();
        let mut combinator;
        let mut index = 0;

        let mut document_state = DocumentState::empty();

        loop {
            let sequence_start = index;

            let mut compound_visitor = CompoundSelectorDependencyCollector {
                classes: SmallVec::new(),
                ids: SmallVec::new(),
                state: ElementState::empty(),
                document_state: &mut document_state,
                other_attributes: false,
                flags: &mut self.flags,
            };

            // Visit all the simple selectors in this sequence.
            //
            // Note that this works because we can't have combinators nested
            // inside simple selectors (i.e. in :not() or :-moz-any()).
            //
            // If we ever support that we'll need to visit nested complex
            // selectors as well, in order to mark them as affecting descendants
            // at least.
            for ss in &mut iter {
                ss.visit(&mut compound_visitor);
                index += 1; // Account for the simple selector.
            }

            for class in compound_visitor.classes {
                self.class_to_selector
                    .try_entry(class, quirks_mode)?
                    .or_insert_with(SmallVec::new)
                    .try_push(Dependency {
                        selector: selector.clone(),
                        selector_offset: sequence_start,
                    })?;
            }

            for id in compound_visitor.ids {
                self.id_to_selector
                    .try_entry(id, quirks_mode)?
                    .or_insert_with(SmallVec::new)
                    .try_push(Dependency {
                        selector: selector.clone(),
                        selector_offset: sequence_start,
                    })?;
            }

            if !compound_visitor.state.is_empty() {
                self.state_affecting_selectors.insert(
                    StateDependency {
                        dep: Dependency {
                            selector: selector.clone(),
                            selector_offset: sequence_start,
                        },
                        state: compound_visitor.state,
                    },
                    quirks_mode,
                )?;
            }

            if compound_visitor.other_attributes {
                self.other_attribute_affecting_selectors.insert(
                    Dependency {
                        selector: selector.clone(),
                        selector_offset: sequence_start,
                    },
                    quirks_mode,
                )?;
            }

            combinator = iter.next_sequence();
            if combinator.is_none() {
                break;
            }

            index += 1; // Account for the combinator.
        }

        if !document_state.is_empty() {
            self.document_state_selectors
                .try_push(DocumentStateDependency {
                    state: document_state,
                    selector: selector.clone(),
                })?;
        }

        Ok(())
    }
}

/// A struct that collects invalidations for a given compound selector.
struct CompoundSelectorDependencyCollector<'a> {
    /// The state this compound selector is affected by.
    state: ElementState,

    /// The document this _complex_ selector is affected by.
    ///
    /// We don't need to track state per compound selector, since it's global
    /// state and it changes for everything.
    document_state: &'a mut DocumentState,

    /// The classes this compound selector is affected by.
    ///
    /// NB: This will be often a single class, but could be multiple in
    /// presence of :not, :-moz-any, .foo.bar.baz, etc.
    classes: SmallVec<[Atom; 5]>,

    /// The IDs this compound selector is affected by.
    ///
    /// NB: This will be almost always a single id, but could be multiple in
    /// presence of :not, :-moz-any, #foo#bar, etc.
    ids: SmallVec<[Atom; 5]>,

    /// Whether it affects other attribute-dependent selectors that aren't ID or
    /// class selectors (NB: We still set this to true in presence of [class] or
    /// [id] attribute selectors).
    other_attributes: bool,

    /// The invalidation map flags, that we set when some attribute selectors are present.
    flags: &'a mut InvalidationMapFlags,
}

impl<'a> SelectorVisitor for CompoundSelectorDependencyCollector<'a> {
    type Impl = SelectorImpl;

    fn visit_simple_selector(&mut self, s: &Component<SelectorImpl>) -> bool {
        #[cfg(feature = "gecko")]
        use crate::selector_parser::NonTSPseudoClass;

        match *s {
            Component::ID(ref id) => {
                self.ids.push(id.clone());
            },
            Component::Class(ref class) => {
                self.classes.push(class.clone());
            },
            Component::NonTSPseudoClass(ref pc) => {
                self.other_attributes |= pc.is_attr_based();
                self.state |= match *pc {
                    #[cfg(feature = "gecko")]
                    NonTSPseudoClass::Dir(ref dir) => dir.element_state(),
                    _ => pc.state_flag(),
                };
                *self.document_state |= pc.document_state_flag();
            },
            _ => {},
        }

        true
    }

    fn visit_attribute_selector(
        &mut self,
        constraint: &NamespaceConstraint<&Namespace>,
        _local_name: &LocalName,
        local_name_lower: &LocalName,
    ) -> bool {
        self.other_attributes = true;
        let may_match_in_no_namespace = match *constraint {
            NamespaceConstraint::Any => true,
            NamespaceConstraint::Specific(ref ns) => ns.is_empty(),
        };

        if may_match_in_no_namespace {
            if *local_name_lower == local_name!("id") {
                self.flags.insert(InvalidationMapFlags::HAS_ID_ATTR_SELECTOR)
            } else if *local_name_lower == local_name!("class") {
                self.flags.insert(InvalidationMapFlags::HAS_CLASS_ATTR_SELECTOR)
            }
        }

        true
    }
}
